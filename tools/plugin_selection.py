#!/usr/bin/env python3
"""Pure, offline model for profile-filtered EDVR plugin selection.

This is groundwork for installer selection. Selection commands do not write
files and do not turn design target defaults into currently usable selections.
The self-test uses and removes temporary fixtures.

Usage:
  python tools/plugin_selection.py --self-test
  python tools/plugin_selection.py --profile vr --empty --dry-run
  python tools/plugin_selection.py --profile vr --request cockpit-visuals
  python tools/plugin_selection.py --profile flat --request temporal-aa --dry-run
"""

import argparse
import copy
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MANIFEST = os.path.join(ROOT, 'src', 'plugins', 'plugin_manifest.json')
PROFILES = ('vr', 'flat')
SELECTABLE_STATUS = 'selection-ready'


class SelectionError(ValueError):
    """A requested plugin set cannot be resolved safely."""


def _plugin_index(manifest):
    if not isinstance(manifest, dict) or not isinstance(manifest.get('plugins'), list):
        raise SelectionError('manifest must contain a plugins array')
    index = {}
    for position, plugin in enumerate(manifest['plugins']):
        if not isinstance(plugin, dict):
            raise SelectionError('plugins[%d] must be an object' % position)
        plugin_id = plugin.get('id')
        if not isinstance(plugin_id, str) or not plugin_id:
            raise SelectionError('plugins[%d].id must be a non-empty string' % position)
        if plugin_id in index:
            raise SelectionError('duplicate plugin ID %s' % plugin_id)
        index[plugin_id] = plugin
    for plugin_id, plugin in index.items():
        for field in ('profiles', 'selectionAvailableProfiles',
                      'recommendedProfiles', 'defaultInstallProfiles'):
            values = plugin.get(field, [])
            if not isinstance(values, list) or any(value not in PROFILES for value in values):
                raise SelectionError('%s.%s must be a profile list containing only vr or flat' %
                                     (plugin_id, field))
            if len(values) != len(set(values)):
                raise SelectionError('%s.%s contains duplicate profiles' % (plugin_id, field))
        supported = set(plugin.get('profiles', []))
        for field in ('selectionAvailableProfiles', 'recommendedProfiles', 'defaultInstallProfiles'):
            if not set(plugin.get(field, [])).issubset(supported):
                raise SelectionError('%s.%s references an unsupported profile' % (plugin_id, field))
        if not set(plugin.get('recommendedProfiles', [])).issubset(
                plugin.get('defaultInstallProfiles', [])):
            raise SelectionError('%s.recommendedProfiles must be a subset of defaultInstallProfiles' %
                                 plugin_id)
        if plugin.get('selectionAvailableProfiles') and plugin.get('implementationStatus') != SELECTABLE_STATUS:
            raise SelectionError('%s is marked selectable without implementationStatus %s' %
                                 (plugin_id, SELECTABLE_STATUS))
        for field in ('dependencies', 'conflicts'):
            values = plugin.get(field, [])
            if not isinstance(values, list):
                raise SelectionError('%s.%s must be an array' % (plugin_id, field))
            for position, item in enumerate(values):
                if not isinstance(item, dict):
                    raise SelectionError('%s.%s[%d] must be an object' %
                                         (plugin_id, field, position))
                other = item.get('id')
                if not isinstance(other, str) or not other or other not in index:
                    raise SelectionError('%s.%s[%d] references unknown plugin ID %r' %
                                         (plugin_id, field, position, other))
                if field == 'dependencies' and item.get('kind') not in ('hard', 'soft'):
                    raise SelectionError('%s.dependencies[%d].kind must be hard or soft' %
                                         (plugin_id, position))
                if field == 'dependencies' and not isinstance(item.get('reason', ''), str):
                    raise SelectionError('%s.dependencies[%d].reason must be a string' %
                                         (plugin_id, position))
    return index


def profile_targets(manifest, profile):
    """Describe design targets and present availability without selecting either."""
    if profile not in PROFILES:
        raise SelectionError('unknown profile %r; expected vr or flat' % profile)
    index = _plugin_index(manifest)
    target_default = []
    recommended = []
    available = []
    for plugin in manifest['plugins']:
        pid = plugin['id']
        supported = plugin.get('profiles', [])
        if profile in plugin.get('defaultInstallProfiles', []):
            target_default.append(pid)
        if profile in plugin.get('recommendedProfiles', []):
            recommended.append(pid)
        if profile in plugin.get('selectionAvailableProfiles', []):
            if profile not in supported:
                raise SelectionError('%s is marked selectable for unsupported profile %s' % (pid, profile))
            available.append(pid)
    return {
        'profile': profile,
        'targetDefaultIds': target_default,
        'recommendedIds': recommended,
        'availableIds': available,
        'availableTargetDefaultIds': [pid for pid in target_default if pid in set(available)],
        'unavailableTargetDefaultIds': [pid for pid in target_default if pid not in set(available)],
    }


def resolve_selection(manifest, profile, requested_ids):
    """Resolve explicit IDs and hard dependencies; report soft deps separately.

    Output IDs follow manifest order. Defaults and recommendations are never
    implicitly added: they describe design targets, while availability is a
    separate, current manifest promise.
    """
    targets = profile_targets(manifest, profile)
    index = _plugin_index(manifest)
    if requested_ids is None:
        raise SelectionError('an explicit plugin selection is required')
    if isinstance(requested_ids, str):
        requested_ids = [requested_ids]
    if not isinstance(requested_ids, (list, tuple)):
        raise SelectionError('requested plugin IDs must be a list')
    available = set(targets['availableIds'])
    requested = []
    for pid in requested_ids:
        if not isinstance(pid, str) or not pid:
            raise SelectionError('requested plugin IDs must be non-empty strings')
        if pid not in index:
            raise SelectionError('unknown plugin ID %s' % pid)
        if pid not in set(plugin['id'] for plugin in manifest['plugins'] if profile in plugin.get('profiles', [])):
            raise SelectionError('%s does not support profile %s' % (pid, profile))
        if pid not in available:
            if index[pid].get('implementationStatus') == 'catalog-only':
                raise SelectionError('%s is catalog-only metadata and is not currently available for selection' % pid)
            raise SelectionError('%s supports profile %s but is not currently available for selection' % (pid, profile))
        if pid not in requested:
            requested.append(pid)

    selected = set(requested)
    visiting = []
    visited = set()

    def add_hard_dependencies(pid):
        if pid in visiting:
            start = visiting.index(pid)
            cycle = visiting[start:] + [pid]
            raise SelectionError('hard dependency cycle: %s' % ' -> '.join(cycle))
        if pid in visited:
            return
        visiting.append(pid)
        plugin = index[pid]
        deps = plugin.get('dependencies', [])
        if not isinstance(deps, list):
            raise SelectionError('%s.dependencies must be an array' % pid)
        for dep in deps:
            if not isinstance(dep, dict):
                raise SelectionError('%s has a malformed dependency entry' % pid)
            did, kind = dep.get('id'), dep.get('kind')
            if not isinstance(did, str) or did not in index:
                raise SelectionError('%s has unknown dependency ID %r' % (pid, did))
            if kind not in ('hard', 'soft'):
                raise SelectionError('%s dependency %s has invalid kind %r' % (pid, did, kind))
            if kind == 'soft':
                continue
            dep_plugin = index[did]
            if profile not in dep_plugin.get('profiles', []):
                raise SelectionError('%s hard dependency %s is unsupported for profile %s' % (pid, did, profile))
            if did not in available:
                if dep_plugin.get('implementationStatus') == 'catalog-only':
                    raise SelectionError('%s hard dependency %s is catalog-only metadata and unavailable for profile %s' %
                                         (pid, did, profile))
                raise SelectionError('%s hard dependency %s is unavailable for profile %s' % (pid, did, profile))
            selected.add(did)
            add_hard_dependencies(did)
        visiting.pop()
        visited.add(pid)

    for pid in requested:
        add_hard_dependencies(pid)

    selected_ordered = [plugin['id'] for plugin in manifest['plugins'] if plugin['id'] in selected]
    for pid in selected_ordered:
        for conflict in index[pid].get('conflicts', []):
            if not isinstance(conflict, dict) or not isinstance(conflict.get('id'), str):
                raise SelectionError('%s has a malformed conflict entry' % pid)
            other = conflict['id']
            if other not in index:
                raise SelectionError('%s has unknown conflict ID %s' % (pid, other))
            if other in selected:
                raise SelectionError('selected plugins %s and %s conflict' % (pid, other))

    soft_missing = set()
    for owner in selected_ordered:
        for dep in index[owner].get('dependencies', []):
            dependency = dep['id']
            if dep['kind'] != 'soft' or dependency in selected:
                continue
            soft_missing.add((owner, dependency, dep.get('reason', '')))
    soft = []
    for owner, dependency, reason in sorted(soft_missing):
        state = 'unavailable' if dependency not in available else 'not selected'
        soft.append({'pluginId': owner, 'dependencyId': dependency,
                     'state': state, 'reason': reason})
    return {
        'profile': profile,
        'requestedIds': [plugin['id'] for plugin in manifest['plugins']
                         if plugin['id'] in requested],
        'selectedIds': selected_ordered,
        'autoSelectedIds': [pid for pid in selected_ordered if pid not in requested],
        'softDependencies': soft,
        'targetDefaultIds': targets['targetDefaultIds'],
        'recommendedIds': targets['recommendedIds'],
    }


def preserve_settings(settings):
    """Return an independent copy for a future Modify flow to carry through."""
    if not isinstance(settings, dict):
        raise SelectionError('settings to preserve must be an object')
    return copy.deepcopy(settings)


def _load_manifest(path):
    with open(path, 'r', encoding='utf-8') as stream:
        return json.load(stream)


def self_test():
    failures = []

    def check(name, condition):
        if not condition:
            failures.append(name)

    base = {
        'plugins': [
            {'id': 'core-aa', 'profiles': ['vr', 'flat'], 'selectionAvailableProfiles': ['vr', 'flat'],
             'implementationStatus': SELECTABLE_STATUS,
             'recommendedProfiles': ['vr'], 'defaultInstallProfiles': ['vr', 'flat'],
             'dependencies': [], 'conflicts': []},
            {'id': 'panel', 'profiles': ['vr'], 'selectionAvailableProfiles': ['vr'],
             'implementationStatus': SELECTABLE_STATUS,
             'recommendedProfiles': ['vr'], 'defaultInstallProfiles': ['vr'],
             'dependencies': [{'id': 'core-aa', 'kind': 'hard', 'reason': 'panel math'},
                              {'id': 'debug', 'kind': 'soft', 'reason': 'optional trace'}],
             'conflicts': []},
            {'id': 'debug', 'profiles': ['vr'], 'selectionAvailableProfiles': [],
             'recommendedProfiles': [], 'defaultInstallProfiles': [],
             'dependencies': [], 'conflicts': []},
            {'id': 'exclusive', 'profiles': ['vr'], 'selectionAvailableProfiles': ['vr'],
             'implementationStatus': SELECTABLE_STATUS,
             'recommendedProfiles': [], 'defaultInstallProfiles': [],
             'dependencies': [], 'conflicts': [{'id': 'panel', 'kind': 'hard'}]},
        ]
    }
    try:
        vr = resolve_selection(base, 'vr', ['panel'])
        check('hard dependency is auto-selected in manifest order',
              vr['selectedIds'] == ['core-aa', 'panel'] and vr['autoSelectedIds'] == ['core-aa'])
        check('soft dependency is reported without forced selection',
              vr['softDependencies'] == [{'pluginId': 'panel', 'dependencyId': 'debug',
                                          'state': 'unavailable', 'reason': 'optional trace'}])
        flat = resolve_selection(base, 'flat', ['core-aa'])
        check('flat profile resolves only available flat plugin', flat['selectedIds'] == ['core-aa'])
        targets = profile_targets(base, 'vr')
        check('default and recommended target metadata is descriptive',
              targets['targetDefaultIds'] == ['core-aa', 'panel'] and
              targets['recommendedIds'] == ['core-aa', 'panel'] and
              targets['unavailableTargetDefaultIds'] == [])

        unavailable_default = copy.deepcopy(base)
        unavailable_default['plugins'][0]['selectionAvailableProfiles'] = []
        target = profile_targets(unavailable_default, 'vr')
        check('unavailable target default is reported but not selected',
              target['targetDefaultIds'] == ['core-aa', 'panel'] and
              target['unavailableTargetDefaultIds'] == ['core-aa'] and
              target['availableTargetDefaultIds'] == ['panel'])
        try:
            resolve_selection(unavailable_default, 'vr', ['core-aa'])
            check('unavailable requested default rejected', False)
        except SelectionError as exc:
            check('unavailable requested default rejected', 'not currently available' in str(exc))

        for name, manifest, profile, requested, expected in (
                ('unknown ID rejected', base, 'vr', ['missing'], 'unknown plugin ID'),
                ('unsupported profile rejected', base, 'flat', ['panel'], 'does not support profile flat'),
                ('conflict rejected', dict(plugins=base['plugins'][:]), 'vr', ['panel', 'exclusive'], 'conflict')):
            try:
                resolve_selection(manifest, profile, requested)
                check(name, False)
            except SelectionError as exc:
                check(name, expected in str(exc))

        cycle = copy.deepcopy(base)
        cycle['plugins'][0]['dependencies'] = [{'id': 'panel', 'kind': 'hard', 'reason': 'cycle'}]
        try:
            resolve_selection(cycle, 'vr', ['panel'])
            check('hard dependency cycle rejected', False)
        except SelectionError as exc:
            check('hard dependency cycle rejected', 'hard dependency cycle' in str(exc))

        bad_dep_availability = copy.deepcopy(base)
        bad_dep_availability['plugins'][0]['selectionAvailableProfiles'] = []
        try:
            resolve_selection(bad_dep_availability, 'vr', ['panel'])
            check('unavailable hard dependency rejected', False)
        except SelectionError as exc:
            check('unavailable hard dependency rejected', 'hard dependency core-aa is unavailable' in str(exc))

        catalog_only = copy.deepcopy(base)
        catalog_only['plugins'][0]['implementationStatus'] = 'catalog-only'
        catalog_only['plugins'][0]['selectionAvailableProfiles'] = ['vr']
        try:
            resolve_selection(catalog_only, 'vr', ['core-aa'])
            check('catalog-only metadata cannot be selected', False)
        except SelectionError as exc:
            check('catalog-only metadata cannot be selected', 'without implementationStatus' in str(exc))

        later_hard = copy.deepcopy(base)
        later_hard['plugins'][2]['selectionAvailableProfiles'] = ['vr']
        later_hard['plugins'][2]['implementationStatus'] = SELECTABLE_STATUS
        later_hard['plugins'][3]['dependencies'] = [{'id': 'debug', 'kind': 'hard'}]
        # A distinct plugin avoids the base panel/exclusive conflict.
        later_hard['plugins'][3]['conflicts'] = []
        resolved = resolve_selection(later_hard, 'vr', ['panel', 'exclusive'])
        check('later hard dependency satisfies earlier soft dependency',
              resolved['softDependencies'] == [] and 'debug' in resolved['autoSelectedIds'])

        for name, field, value, expected in (
                ('unsupported default rejected', 'defaultInstallProfiles', ['flat'], 'unsupported profile'),
                ('unsupported recommendation rejected', 'recommendedProfiles', ['flat'], 'unsupported profile'),
                ('duplicate profiles rejected', 'profiles', ['vr', 'vr'], 'duplicate profiles'),
                ('recommendation outside defaults rejected', 'defaultInstallProfiles', [], 'subset'),
                ('unknown selectable status rejected', 'implementationStatus', 'unknown', 'without implementationStatus'),
                ('malformed dependency reason rejected', 'dependencies',
                 [{'id': 'debug', 'kind': 'soft', 'reason': {'bad': 'reason'}}], 'reason must be a string')):
            malformed = copy.deepcopy(base)
            malformed['plugins'][1][field] = value
            try:
                resolve_selection(malformed, 'vr', ['panel'])
                check(name, False)
            except SelectionError as exc:
                check(name, expected in str(exc))

        empty = resolve_selection(base, 'vr', [])
        check('explicit empty selection stays empty',
              empty['requestedIds'] == [] and empty['selectedIds'] == [] and empty['softDependencies'] == [])

        current = {'user': {'advanced.x': True}, 'unknownLegacyKey': 'keep'}
        carried = preserve_settings(current)
        carried['user']['advanced.x'] = False
        check('Modify settings model returns independent preserved settings',
              current['user']['advanced.x'] is True and carried['unknownLegacyKey'] == 'keep')

        # Exercise the actual CLI dry-run with a temporary synthetic manifest.
        import tempfile
        with tempfile.TemporaryDirectory(dir=ROOT) as temp_dir:
            path = os.path.join(temp_dir, 'manifest.json')
            with open(path, 'w', encoding='utf-8') as stream:
                json.dump(base, stream)
            before = {}
            for name in os.listdir(temp_dir):
                with open(os.path.join(temp_dir, name), 'rb') as stream:
                    before[name] = stream.read()
            code = main(['--manifest', path, '--profile', 'vr', '--request', 'panel', '--dry-run'])
            after = {}
            for name in os.listdir(temp_dir):
                with open(os.path.join(temp_dir, name), 'rb') as stream:
                    after[name] = stream.read()
            check('--dry-run creates or changes no files', code == 0 and before == after)
            empty_code = main(['--manifest', path, '--profile', 'vr', '--empty', '--dry-run'])
            check('CLI explicit empty selection', empty_code == 0)
            after_empty = {}
            for name in os.listdir(temp_dir):
                with open(os.path.join(temp_dir, name), 'rb') as stream:
                    after_empty[name] = stream.read()
            check('empty --dry-run creates or changes no files', after_empty == before)
    except Exception as exc:
        failures.append('unexpected self-test error: %s' % exc)

    if failures:
        print('plugin_selection: self-test FAILED')
        for failure in failures:
            print('  - ' + failure)
        return 1
    print('plugin_selection: self-test OK')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', default=DEFAULT_MANIFEST)
    parser.add_argument('--profile', choices=PROFILES)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument('--request', action='append', dest='requested_ids')
    selection.add_argument('--empty', action='store_true',
                           help='explicitly select no optional plugins')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--dry-run', action='store_true',
                        help='resolve and print the model; writes no files')
    args = parser.parse_args(argv)
    if args.self_test and args.dry_run:
        parser.error('--self-test creates temporary fixtures and cannot be combined with --dry-run')
    if args.self_test:
        return self_test()
    if not args.profile:
        parser.error('--profile is required unless --self-test is used')
    if not args.requested_ids and not args.empty:
        parser.error('an explicit --request or --empty is required')
    try:
        manifest = _load_manifest(args.manifest)
        result = resolve_selection(manifest, args.profile, [] if args.empty else args.requested_ids)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print('plugin_selection: %s' % exc, file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, ensure_ascii=False))
    if args.dry_run:
        print('plugin_selection: dry-run; no files written')
    return 0


if __name__ == '__main__':
    sys.exit(main())
