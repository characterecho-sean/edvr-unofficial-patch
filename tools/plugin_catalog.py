#!/usr/bin/env python3
"""Validate and export EDVR's canonical first-party plugin catalog.

The manifest is deliberately data-only. First-party callbacks and dispatch
state live in C++ registries keyed by the stable manifest index/ID.

Usage:
  python tools/plugin_catalog.py --self-test
  python tools/plugin_catalog.py [--emit-cpp PATH] [--emit-json PATH] [--dry-run]

--dry-run performs validation and reports the output sizes without creating
files or directories.
"""

import copy
import json
import math
import re
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, 'src', 'plugins', 'plugin_manifest.json')
EXPECTED_IDS = (
    'temporal-aa', 'cockpit-visuals', 'exposure', 'scanners', 'intro',
    'on-foot-panel', 'comfort', 'performance', 'diagnostics',
)
PROFILES = ('vr', 'flat')
METRIC_SPECS = {
    'cpu': {
        'unit': 'milliseconds per frame',
        'coverage': ('EDVR-owned CPU work attributable to this plugin. The existing draw-hook CPU census is aggregate and does not split module cost.'),
    },
    'issuedGpuWork': {
        'unit': 'EDVR-issued D3D11 calls per frame',
        'coverage': ('Calls issued by this plugin only; forwarded game calls and shared/core work are excluded unless explicitly attributed.'),
    },
    'directGpu': {
        'unit': 'GPU milliseconds per frame',
        'coverage': ('Direct GPU work attributable to this plugin. Whole-frame GPU samples are not plugin-level attribution.'),
    },
}
METRIC_IDS = tuple(METRIC_SPECS)

# Match values emitted by `git describe --tags --always --dirty`, the version
# identity written into EDVR's version log line. A bare tag/hash may have the
# standard distance/hash suffix and optional dirty marker.
_BUILD_ID_RE = re.compile(
    r'^(?:v?[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?|[0-9a-f]{7,40})'
    r'(?:-[0-9]+-g[0-9a-f]{7,40})?(?:-dirty)?$')
EXPECTED_PROFILES = {
    'temporal-aa': ('vr', 'flat'),
    'cockpit-visuals': ('vr',),
    'exposure': ('vr',),
    'scanners': ('vr',),
    'intro': ('vr',),
    'on-foot-panel': ('vr',),
    'comfort': ('vr',),
    'performance': ('vr',),
    'diagnostics': ('vr', 'flat'),
}
EXPECTED_IMPLEMENTATION = {
    'temporal-aa': 'catalog-only',
    'cockpit-visuals': 'phase1-pilot',
    'exposure': 'catalog-only',
    'scanners': 'catalog-only',
    'intro': 'catalog-only',
    'on-foot-panel': 'catalog-only',
    'comfort': 'catalog-only',
    'performance': 'catalog-only',
    'diagnostics': 'catalog-only',
}
DEFAULT_INSTALL = {
    'vr': frozenset(EXPECTED_IDS[:-1]),
    'flat': frozenset(('temporal-aa',)),
}
RECOMMENDED = {
    'vr': frozenset(EXPECTED_IDS[:-1]),
    'flat': frozenset(('temporal-aa',)),
}
RUNTIME_KINDS = frozenset(('none', 'partial', 'required'))


def load_json(path=MANIFEST):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def ini_documented_keys(path=None):
    """Read setting names from edvr.ini, including commented template keys."""
    path = path or os.path.join(ROOT, 'edvr.ini')
    keys = set()
    section = ''
    with open(path, encoding='utf-8-sig', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if line.startswith('[') and ']' in line:
                section = line[1:line.index(']')].strip()
                continue
            body = line[1:].strip() if line.startswith(('#', ';')) else line
            if line.startswith(('#', ';')) and ' ' in body.split('=')[0].strip():
                continue
            if '=' not in body:
                continue
            name = body.split('=', 1)[0].strip()
            if not name or any(ch not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_' for ch in name):
                continue
            keys.add((section + '.' if section else '') + name)
    return keys


def _string_list(value, label, problems, unique=True):
    if not isinstance(value, list) or any(not isinstance(x, str) or not x for x in value):
        problems.append('%s must be an array of non-empty strings' % label)
        return []
    if unique and len(value) != len(set(value)):
        problems.append('%s contains duplicate values' % label)
    return value


def _finite_number(value):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return False
    try:
        converted = float(value)
    except (OverflowError, TypeError, ValueError):
        return False
    return math.isfinite(converted)


def _validate_cost_budget(budget, plugin_id, supported_profiles):
    """Validate honest unmeasured declarations and provenance-bound relative budgets."""
    problems = []
    label = plugin_id + '.costBudget'
    if not isinstance(budget, dict) or set(budget) != {'state', 'reference', 'metrics'}:
        return [label + ' must define exactly state, reference, and metrics']

    metrics = budget.get('metrics')
    if not isinstance(metrics, dict) or set(metrics) != set(METRIC_IDS):
        problems.append(label + '.metrics must define exactly %s' % ', '.join(METRIC_IDS))
        metrics = {}

    states = set()
    for metric_id, spec in METRIC_SPECS.items():
        metric = metrics.get(metric_id)
        mlabel = '%s.metrics.%s' % (label, metric_id)
        if not isinstance(metric, dict):
            problems.append(mlabel + ' must be an object')
            continue
        state = metric.get('state')
        if state not in ('unmeasured', 'relative'):
            problems.append(mlabel + '.state must be unmeasured or relative')
            continue
        states.add(state)
        if metric.get('unit') != spec['unit']:
            problems.append(mlabel + '.unit must be %s' % spec['unit'])
        if metric.get('coverage') != spec['coverage']:
            problems.append(mlabel + '.coverage must preserve the documented attribution scope')
        if state == 'unmeasured':
            if set(metric) != {'state', 'unit', 'coverage'}:
                problems.append(mlabel + ' unmeasured entries cannot carry values, limits, or uncertainty')
        elif set(metric) != {'state', 'unit', 'coverage', 'referenceValue', 'maxRelativeIncrease'}:
            problems.append(mlabel + ' relative entries require referenceValue and maxRelativeIncrease')
        else:
            reference_value = metric.get('referenceValue')
            limit = metric.get('maxRelativeIncrease')
            if not _finite_number(reference_value) or reference_value <= 0:
                problems.append(mlabel + '.referenceValue must be a finite positive measured baseline')
            if not _finite_number(limit) or limit < 0:
                problems.append(mlabel + '.maxRelativeIncrease must be a finite non-negative relative limit')

    state = budget.get('state')
    expected_state = ('unmeasured' if states == {'unmeasured'} else
                      'relative' if states == {'relative'} else
                      'mixed' if states == {'unmeasured', 'relative'} else None)
    if state not in ('unmeasured', 'relative', 'mixed') or state != expected_state:
        problems.append(label + '.state must match the metric measurement states')

    reference = budget.get('reference')
    if expected_state == 'unmeasured':
        if reference is not None:
            problems.append(label + '.reference must be null while every metric is unmeasured')
    elif expected_state in ('relative', 'mixed'):
        required = {'measurementId', 'build', 'profile', 'scene', 'config', 'uncertainty'}
        if not isinstance(reference, dict) or set(reference) != required:
            problems.append(label + '.reference must define measurementId, build, profile, scene, config, uncertainty')
        else:
            for field in ('measurementId', 'build', 'scene'):
                if not isinstance(reference[field], str) or not reference[field].strip():
                    problems.append(label + '.reference.%s must be a non-empty string' % field)
            build_id = reference.get('build')
            if (not isinstance(build_id, str) or not _BUILD_ID_RE.fullmatch(build_id) or
                    build_id.lower() in ('unknown', 'unversioned-test-build')):
                problems.append(label + '.reference.build must match a logged git-describe version identity')
            if reference['profile'] not in PROFILES:
                problems.append(label + '.reference.profile must be a known runtime profile')
            elif reference['profile'] not in supported_profiles:
                problems.append(label + '.reference.profile is unsupported by this plugin')
            config = reference['config']
            if not isinstance(config, dict) or not config:
                problems.append(label + '.reference.config must identify a non-empty measurement configuration')
            elif any(not isinstance(key, str) or not key.strip() or
                     not isinstance(value, (str, int, float, bool)) or
                     (isinstance(value, float) and not _finite_number(value))
                     for key, value in config.items()):
                problems.append(label + '.reference.config must contain named scalar settings')

            uncertainty = reference['uncertainty']
            required_uncertainty = {'method', 'sampleCount', 'confidence', 'metrics'}
            if not isinstance(uncertainty, dict) or set(uncertainty) != required_uncertainty:
                problems.append(label + '.reference.uncertainty must define method, sampleCount, confidence, metrics')
            else:
                if not isinstance(uncertainty['method'], str) or not uncertainty['method'].strip():
                    problems.append(label + '.reference.uncertainty.method must be non-empty')
                sample_count = uncertainty['sampleCount']
                if type(sample_count) is not int or not 2 <= sample_count <= 0xFFFFFFFF:
                    problems.append(label + '.reference.uncertainty.sampleCount must fit uint32 and be >= 2')
                confidence = uncertainty['confidence']
                if not _finite_number(confidence) or not 0 < confidence < 1:
                    problems.append(label + '.reference.uncertainty.confidence must be between 0 and 1')
                intervals = uncertainty['metrics']
                measured_ids = {metric_id for metric_id, metric in metrics.items()
                                if isinstance(metric, dict) and metric.get('state') == 'relative'}
                if not isinstance(intervals, dict) or set(intervals) != measured_ids:
                    problems.append(label + '.reference.uncertainty.metrics must match relative metric IDs')
                else:
                    for metric_id in measured_ids:
                        interval = intervals[metric_id]
                        metric = metrics[metric_id]
                        mlabel = '%s.metrics.%s' % (label, metric_id)
                        ilabel = '%s.reference.uncertainty.metrics.%s' % (label, metric_id)
                        if not isinstance(interval, dict) or set(interval) != {'low', 'high', 'unit'}:
                            problems.append(ilabel + ' must define low, high, and unit')
                            continue
                        low, high = interval['low'], interval['high']
                        if interval['unit'] != METRIC_SPECS[metric_id]['unit']:
                            problems.append(ilabel + '.unit does not match the metric unit')
                        if (not _finite_number(low) or not _finite_number(high) or low < 0 or
                                low > high or not _finite_number(metric.get('referenceValue')) or
                                not low <= metric['referenceValue'] <= high):
                            problems.append(ilabel + ' must be a finite ordered interval containing the measured referenceValue')
                        elif (_finite_number(metric.get('maxRelativeIncrease')) and
                              _finite_number(metric.get('referenceValue')) and
                              metric['referenceValue'] > 0):
                            # The only supported allowance is the positive
                            # one-sided margin already present in this metric's
                            # measured reference interval: (high / baseline)-1.
                            # This encodes "within measured noise" without an
                            # arbitrary independent regression threshold.
                            noise_margin = (high / metric['referenceValue']) - 1.0
                            if not _finite_number(noise_margin):
                                problems.append(ilabel + ' produces a non-finite relative uncertainty margin')
                            elif metric['maxRelativeIncrease'] > noise_margin:
                                problems.append(mlabel + '.maxRelativeIncrease exceeds the reference uncertainty upper margin')
    elif reference is not None:
        problems.append(label + '.reference must be null when no metrics are measured')
    return problems


def validate_manifest(data, documented_keys=None):
    """Return human-readable errors; validate refs, ownership, and defaults."""
    problems = []
    if not isinstance(data, dict):
        return ['manifest must be a JSON object']
    if type(data.get('schemaVersion')) is not int or data.get('schemaVersion') != 2:
        problems.append('unsupported schemaVersion (expected integer 2)')
    if not isinstance(data.get('description'), str) or not data.get('description'):
        problems.append('description must be a non-empty string')
    if not isinstance(data.get('selectionSemantics'), str) or not data.get('selectionSemantics'):
        problems.append('selectionSemantics must explain design defaults versus current availability')
    profiles = _string_list(data.get('profiles'), 'profiles', problems)
    if tuple(profiles) != PROFILES:
        problems.append('profiles must be exactly %s in canonical order' % (', '.join(PROFILES)))

    core_keys = _string_list(data.get('coreOwnedConfigKeys'), 'coreOwnedConfigKeys', problems)
    plugins = data.get('plugins')
    if not isinstance(plugins, list):
        problems.append('plugins must be an array')
        plugins = []
    ids = []
    by_id = {}
    ownership = {}
    for key in core_keys:
        ownership.setdefault(key, []).append('core')

    for position, plugin in enumerate(plugins):
        label = 'plugins[%d]' % position
        if not isinstance(plugin, dict):
            problems.append('%s must be an object' % label)
            continue
        pid = plugin.get('id')
        if not isinstance(pid, str) or not pid:
            problems.append('%s.id must be a non-empty string' % label)
            continue
        ids.append(pid)
        if pid in by_id:
            problems.append('duplicate plugin id: %s' % pid)
        by_id[pid] = plugin
        if type(plugin.get('index')) is not int or plugin.get('index') != position:
            problems.append('%s.index must equal its canonical array position %d' % (pid, position))
        for field in ('name', 'description', 'costNote'):
            if not isinstance(plugin.get(field), str) or not plugin.get(field):
                problems.append('%s.%s must be a non-empty string' % (pid, field))
        supported = _string_list(plugin.get('profiles'), pid + '.profiles', problems)
        problems.extend(_validate_cost_budget(plugin.get('costBudget'), pid, supported))
        recommended = _string_list(plugin.get('recommendedProfiles'), pid + '.recommendedProfiles', problems)
        defaults = _string_list(plugin.get('defaultInstallProfiles'), pid + '.defaultInstallProfiles', problems)
        expected_profiles = EXPECTED_PROFILES.get(pid)
        if expected_profiles is None:
            problems.append('unknown plugin id: %s' % pid)
        elif tuple(supported) != expected_profiles:
            problems.append('%s.profiles must be %s' % (pid, ', '.join(expected_profiles)))
        status = plugin.get('implementationStatus')
        if status != EXPECTED_IMPLEMENTATION.get(pid):
            problems.append('%s.implementationStatus must be %s' %
                            (pid, EXPECTED_IMPLEMENTATION.get(pid, 'a known implementation phase')))
        selectable = _string_list(plugin.get('selectionAvailableProfiles'), pid + '.selectionAvailableProfiles', problems)
        for profile in selectable:
            if profile not in supported:
                problems.append('%s.selectionAvailableProfiles references unsupported profile %s' % (pid, profile))
        if selectable:
            problems.append('%s selection is not available during Phase 1' % pid)
        for field, values in (('profiles', supported), ('recommendedProfiles', recommended),
                              ('defaultInstallProfiles', defaults)):
            for profile in values:
                if profile not in profiles:
                    problems.append('%s.%s references unknown profile %s' % (pid, field, profile))
                elif profile not in supported:
                    problems.append('%s.%s references unsupported profile %s' % (pid, field, profile))
        if not set(recommended).issubset(set(defaults)):
            problems.append('%s recommendedProfiles must be a subset of defaultInstallProfiles' % pid)
        if pid in DEFAULT_INSTALL:
            expected_defaults = tuple(p for p in PROFILES if pid in DEFAULT_INSTALL[p])
            if set(defaults) != set(expected_defaults):
                problems.append('%s.defaultInstallProfiles must be %s' %
                                (pid, ', '.join(expected_defaults) or '(empty)'))
            expected_recommended = tuple(p for p in PROFILES if pid in RECOMMENDED[p])
            if set(recommended) != set(expected_recommended):
                problems.append('%s.recommendedProfiles must be %s' %
                                (pid, ', '.join(expected_recommended) or '(empty)'))

        runtime = plugin.get('runtimeRequirement')
        if not isinstance(runtime, dict) or set(runtime) != set(PROFILES):
            problems.append('%s.runtimeRequirement must define vr and flat' % pid)
            runtime = {}
        for profile in PROFILES:
            value = runtime.get(profile)
            if value not in RUNTIME_KINDS:
                problems.append('%s.runtimeRequirement.%s must be none, partial, or required' % (pid, profile))
            if profile not in supported and value != 'none':
                problems.append('%s has a runtime requirement for unsupported profile %s' % (pid, profile))
            if profile == 'flat' and value != 'none':
                problems.append('%s flat runtimeRequirement must be none' % pid)

        owned = _string_list(plugin.get('ownedConfigKeys'), pid + '.ownedConfigKeys', problems)
        for key in owned:
            ownership.setdefault(key, []).append(pid)
        for field in ('hookPoints', 'rigs'):
            _string_list(plugin.get(field), pid + '.' + field, problems)
        claims = plugin.get('claims')
        if not isinstance(claims, list):
            problems.append(pid + '.claims must be an array')
            claims = []
        claim_ids = set()
        for ci, claim in enumerate(claims):
            clabel = '%s.claims[%d]' % (pid, ci)
            if not isinstance(claim, dict):
                problems.append(clabel + ' must be an object')
                continue
            cid = claim.get('id')
            if not isinstance(cid, str) or not cid:
                problems.append(clabel + '.id must be a non-empty string')
            elif cid in claim_ids:
                problems.append('%s has duplicate claim id %s' % (pid, cid))
            else:
                claim_ids.add(cid)
            if not isinstance(claim.get('hook'), str) or not claim.get('hook'):
                problems.append(clabel + '.hook must be a non-empty string')
            if type(claim.get('precedence')) is not int:
                problems.append(clabel + '.precedence must be an integer')
            if not isinstance(claim.get('rig'), str):
                problems.append(clabel + '.rig must be a string')
            if not isinstance(claim.get('shape'), str):
                problems.append(clabel + '.shape must be a string')
            draw_shape = claim.get('drawShape')
            if not isinstance(draw_shape, dict) or set(draw_shape) != {'kind', 'count', 'instances'}:
                problems.append(clabel + '.drawShape must define exactly kind, count, and instances')
            else:
                kind = draw_shape.get('kind')
                count = draw_shape.get('count')
                instances = draw_shape.get('instances')
                if not isinstance(kind, str) or len(kind) != 1 or kind not in ('D', 'I', 'N', 'X'):
                    problems.append(clabel + '.drawShape.kind must be one of D, I, N, X')
                if type(count) is not int or count < 0 or count > 0xffffffff:
                    problems.append(clabel + '.drawShape.count must be an unsigned 32-bit integer')
                if type(instances) is not int or instances < 1 or instances > 0xffffffff:
                    problems.append(clabel + '.drawShape.instances must be a positive 32-bit integer')
            for relation in ('precedenceAfter', 'precedenceBefore'):
                if relation in claim and (not isinstance(claim[relation], str) or not claim[relation]):
                    problems.append(clabel + '.' + relation + ' must be a non-empty string')
            shader_pairs = claim.get('shaderPairs')
            if not isinstance(shader_pairs, list):
                problems.append(clabel + '.shaderPairs must be an array')
            else:
                seen_pairs = set()
                for si, pair in enumerate(shader_pairs):
                    if not isinstance(pair, dict) or not isinstance(pair.get('vs'), str) or not isinstance(pair.get('ps'), str):
                        problems.append('%s.shaderPairs[%d] must define string vs and ps hashes' % (clabel, si))
                        continue
                    signature = (pair['vs'].upper(), pair['ps'].upper())
                    if len(signature[0]) != 16 or len(signature[1]) != 16 or any(
                            ch not in '0123456789ABCDEF' for value in signature for ch in value):
                        problems.append('%s.shaderPairs[%d] hashes must be 16 hex digits' % (clabel, si))
                    if signature in seen_pairs:
                        problems.append('%s has duplicate shader pair %s/%s' % (pid, pair['vs'], pair['ps']))
                    seen_pairs.add(signature)

        deps = plugin.get('dependencies')
        if not isinstance(deps, list):
            problems.append(pid + '.dependencies must be an array')
            deps = []
        seen_deps = set()
        for di, dep in enumerate(deps):
            dlabel = '%s.dependencies[%d]' % (pid, di)
            if not isinstance(dep, dict):
                problems.append(dlabel + ' must be an object')
                continue
            did, kind = dep.get('id'), dep.get('kind')
            if not isinstance(did, str) or not did:
                problems.append(dlabel + '.id must be a non-empty string')
                continue
            if did == pid:
                problems.append('%s cannot depend on itself' % pid)
            if did in seen_deps:
                problems.append('%s has duplicate dependency %s' % (pid, did))
            seen_deps.add(did)
            if kind not in ('hard', 'soft'):
                problems.append(dlabel + '.kind must be hard or soft')
            if not isinstance(dep.get('reason'), str) or not dep.get('reason'):
                problems.append(dlabel + '.reason must be a non-empty string')

        conflicts = plugin.get('conflicts')
        if not isinstance(conflicts, list):
            problems.append(pid + '.conflicts must be an array')
            conflicts = []
        seen_conflicts = set()
        for ci, conflict in enumerate(conflicts):
            clabel = '%s.conflicts[%d]' % (pid, ci)
            if not isinstance(conflict, dict):
                problems.append(clabel + ' must be an object')
                continue
            cid, kind = conflict.get('id'), conflict.get('kind')
            if not isinstance(cid, str) or not cid:
                problems.append(clabel + '.id must be a non-empty string')
            elif cid == pid:
                problems.append('%s cannot conflict with itself' % pid)
            elif cid in seen_conflicts:
                problems.append('%s has duplicate conflict %s' % (pid, cid))
            else:
                seen_conflicts.add(cid)
            if kind not in ('hard', 'soft'):
                problems.append(clabel + '.kind must be hard or soft')

    if tuple(ids) != EXPECTED_IDS:
        problems.append('plugin IDs/order must be exactly %s' % ', '.join(EXPECTED_IDS))
    pilot = by_id.get('cockpit-visuals', {})
    expected_pilot_claim = {
        'id': 'night-vision',
        'hook': 'draw.classify',
        'precedence': 100,
        'precedenceAfter': 'census.skip-ranges',
        'precedenceBefore': 'legacy.remlok',
        'rig': 'night_vision_test',
        'shaderPairs': [{'vs': 'FCF7BD2896751D96', 'ps': 'F786D34B5E118D5E'}],
        'drawShape': {'kind': 'X', 'count': 240, 'instances': 1},
    }
    pilot_claims = pilot.get('claims', []) if isinstance(pilot, dict) else []
    if not any(all(claim.get(key) == value for key, value in expected_pilot_claim.items())
               for claim in pilot_claims if isinstance(claim, dict)):
        problems.append('cockpit-visuals must preserve the Phase 1 night-vision claim metadata')
    pilot_rigs = pilot.get('rigs', []) if isinstance(pilot, dict) else []
    if not all(rig in pilot_rigs for rig in ('night_vision_test', 'plugin_dispatch_test')):
        problems.append('cockpit-visuals rigs must name night_vision_test and plugin_dispatch_test')
    id_set = set(ids)
    graph = {pid: [] for pid in ids}
    for pid, plugin in by_id.items():
        for dep in plugin.get('dependencies', []) if isinstance(plugin.get('dependencies'), list) else []:
            if not isinstance(dep, dict):
                continue
            did = dep.get('id')
            if did not in id_set:
                problems.append('%s dependency references unknown plugin %s' % (pid, did))
            else:
                graph[pid].append(did)
                if dep.get('kind') == 'hard':
                    dependent_defaults = set(plugin.get('defaultInstallProfiles', []))
                    dependency_defaults = set(by_id.get(did, {}).get('defaultInstallProfiles', []))
                    if not dependent_defaults.issubset(dependency_defaults):
                        problems.append('%s hard dependency %s is absent from one of its target default profiles' %
                                        (pid, did))
                    selectable = plugin.get('selectionAvailableProfiles', [])
                    dependent_available = set(selectable) if isinstance(selectable, list) else set()
                    dependency = by_id.get(did, {})
                    dep_selectable = dependency.get('selectionAvailableProfiles', [])
                    dependency_available = set(dep_selectable) if isinstance(dep_selectable, list) else set()
                    dep_profiles = dependency.get('profiles', [])
                    dependency_profiles = set(dep_profiles) if isinstance(dep_profiles, list) else set()
                    if not dependent_available.issubset(dependency_profiles):
                        problems.append('%s hard dependency %s is unsupported in a selectable profile' %
                                        (pid, did))
                    if not dependent_available.issubset(dependency_available):
                        problems.append('%s hard dependency %s is unavailable in a selectable profile' %
                                        (pid, did))
        for conflict in plugin.get('conflicts', []) if isinstance(plugin.get('conflicts'), list) else []:
            if isinstance(conflict, dict) and conflict.get('id') not in id_set:
                problems.append('%s conflict references unknown plugin %s' % (pid, conflict.get('id')))
    visiting, visited = set(), set()

    def visit(pid):
        if pid in visiting:
            problems.append('dependency cycle includes %s' % pid)
            return
        if pid in visited:
            return
        visiting.add(pid)
        for child in graph.get(pid, ()):
            visit(child)
        visiting.remove(pid)
        visited.add(pid)

    for pid in graph:
        visit(pid)

    for key, owners in sorted(ownership.items()):
        if len(owners) > 1:
            problems.append('duplicate config ownership for %s: %s' % (key, ', '.join(owners)))
    if documented_keys is not None:
        documented = set(documented_keys)
        for key in sorted(set(ownership) - documented):
            problems.append('config ownership references unknown key: %s' % key)
        for key in sorted(documented - set(ownership)):
            problems.append('config key has no owner: %s' % key)
    return problems


def profile_mask(profile_names):
    return sum(1 << PROFILES.index(name) for name in profile_names)


def _cpp_string(value):
    return json.dumps(value, ensure_ascii=True)


def render_cpp(data):
    """Render the static, allocation-free C-compatible metadata tables."""
    lines = [
        '// Generated by tools/plugin_catalog.py -- do not edit.',
        '// Array position and index are the stable first-party plugin ID/mask bit.',
        '#pragma once',
        '#include <cstdint>',
        'namespace edvr { namespace plugins {',
        'enum ProfileMask : std::uint32_t {',
        '    kProfileNone = 0,',
        '    kProfileVr = 1u << 0,',
        '    kProfileFlat = 1u << 1,',
        '    kProfileLegacyVr = kProfileVr,',
        '};',
        'enum CostBudgetState : std::uint32_t { kCostBudgetUnmeasured = 0, kCostBudgetRelative = 1, kCostBudgetMixed = 2 };',
        'enum CostMetricState : std::uint32_t { kCostMetricUnmeasured = 0, kCostMetricRelative = 1 };',
        'struct CostMetric { const char* id; std::uint32_t state; const char* unit; const char* coverage; const double* referenceValue; const double* maxRelativeIncrease; };',
        'struct CostMetricUncertainty { const char* id; double low, high; const char* unit; };',
        'struct CostReference { const char* measurementId; const char* build; std::uint32_t profileMask; const char* scene; const char* configJson; const char* uncertaintyMethod; std::uint32_t sampleCount; double confidence; const CostMetricUncertainty* metrics; std::uint32_t metricCount; };',
        'struct CostBudget { std::uint32_t state; const CostReference* reference; const CostMetric* metrics; std::uint32_t metricCount; };',
        'struct Dependency { const char* id; std::uint32_t kind; const char* reason; };',
        'struct ShaderPair { std::uint64_t vertexShaderHash, pixelShaderHash; };',
        'struct DrawShape { std::uint8_t kind; std::uint32_t count, instances; };',
        'struct Claim { const char* id; const char* hook; std::int32_t precedence; const char* precedenceAfter; const char* precedenceBefore; const char* rig; const char* shape; DrawShape drawShape; const ShaderPair* shaderPairs; std::uint32_t shaderPairCount; };',
        'struct PluginRecord {',
        '    std::uint32_t index;',
        '    const char* id; const char* name; const char* description;',
        '    const char* implementationStatus;',
        '    // Target architecture defaults; use selectionAvailableProfileMask for current availability.',
        '    std::uint32_t profileMask, recommendedProfileMask, defaultInstallProfileMask;',
        '    std::uint32_t selectionAvailableProfileMask;',
        '    std::uint32_t runtimeRequiredProfileMask, runtimePartialProfileMask;',
        '    const char* const* ownedConfigKeys; std::uint32_t ownedConfigKeyCount;',
        '    const char* const* hookPoints; std::uint32_t hookPointCount;',
        '    const Claim* claims; std::uint32_t claimCount;',
        '    const Dependency* dependencies; std::uint32_t dependencyCount;',
        '    const char* const* rigs; std::uint32_t rigCount;',
        '    const char* costNote;',
        '    const CostBudget* costBudget;',
        '};',
    ]
    lines.append('inline constexpr const char* kCoreOwnedConfigKeys[] = {')
    lines.extend('    %s,' % _cpp_string(value) for value in data['coreOwnedConfigKeys'])
    lines.append('};')
    lines.append('inline constexpr std::uint32_t kCoreOwnedConfigKeyCount = %d;' %
                 len(data['coreOwnedConfigKeys']))
    for plugin in data['plugins']:
        pid = plugin['id'].replace('-', '_')
        for ci, claim in enumerate(plugin['claims']):
            pairs = claim['shaderPairs']
            lines.append('inline constexpr ShaderPair k_%s_claim_%d_shader_pairs[] = {' % (pid, ci))
            for pair in pairs:
                lines.append('    {0x%sull, 0x%sull},' % (pair['vs'], pair['ps']))
            if not pairs:
                lines.append('    {0ull, 0ull},')
            lines.append('};')
            claim_symbol = ''.join(part.capitalize() for part in claim['id'].replace('_', '-').split('-'))
            plugin_symbol = ''.join(part.capitalize() for part in plugin['id'].replace('_', '-').split('-'))
            lines.append('enum : std::uint32_t { kClaim%s%s = %d };' %
                         (plugin_symbol, claim_symbol, ci))
        for field, key in (('owned_config_keys', 'ownedConfigKeys'), ('hook_points', 'hookPoints'), ('rigs', 'rigs')):
            values = plugin[key]
            lines.append('inline constexpr const char* k_%s_%s[] = {' % (pid, field))
            lines.extend('    %s,' % _cpp_string(value) for value in values)
            if not values:
                lines.append('    "",')
            lines.append('};')
        lines.append('inline constexpr Claim k_%s_claims[] = {' % pid)
        for ci, claim in enumerate(plugin['claims']):
            shape = claim['drawShape']
            lines.append('    {%s, %s, %d, %s, %s, %s, %s, {0x%02Xu, %du, %du}, k_%s_claim_%d_shader_pairs, %d},' % (
                _cpp_string(claim['id']), _cpp_string(claim['hook']), claim['precedence'],
                _cpp_string(claim.get('precedenceAfter', '')), _cpp_string(claim.get('precedenceBefore', '')),
                _cpp_string(claim['rig']), _cpp_string(claim['shape']), ord(shape['kind']),
                shape['count'], shape['instances'], pid, ci,
                len(claim['shaderPairs'])))
        if not plugin['claims']:
            lines.append('    {"", "", 0, "", "", "", "", {0, 0, 0}, nullptr, 0},')
        lines.append('};')
        lines.append('inline constexpr Dependency k_%s_dependencies[] = {' % pid)
        for dep in plugin['dependencies']:
            lines.append('    {%s, %d, %s},' % (
                _cpp_string(dep['id']), 1 if dep['kind'] == 'hard' else 2, _cpp_string(dep['reason'])))
        if not plugin['dependencies']:
            lines.append('    {"", 0, ""},')
        lines.append('};')
        budget = plugin['costBudget']
        metrics = budget['metrics']
        for metric_id in METRIC_IDS:
            metric = metrics[metric_id]
            if metric['state'] == 'relative':
                lines.append('inline constexpr double k_%s_cost_%s_reference_value = %.17g;' % (
                    pid, metric_id, metric['referenceValue']))
                lines.append('inline constexpr double k_%s_cost_%s_relative_limit = %.17g;' % (
                    pid, metric_id, metric['maxRelativeIncrease']))
        lines.append('inline constexpr CostMetric k_%s_cost_metrics[] = {' % pid)
        for metric_id in METRIC_IDS:
            metric = metrics[metric_id]
            metric_state = 'kCostMetricRelative' if metric['state'] == 'relative' else 'kCostMetricUnmeasured'
            reference_ptr = '&k_%s_cost_%s_reference_value' % (pid, metric_id) if metric['state'] == 'relative' else 'nullptr'
            limit_ptr = '&k_%s_cost_%s_relative_limit' % (pid, metric_id) if metric['state'] == 'relative' else 'nullptr'
            lines.append('    {%s, %s, %s, %s, %s, %s},' % (
                _cpp_string(metric_id), metric_state, _cpp_string(metric['unit']),
                _cpp_string(metric['coverage']), reference_ptr, limit_ptr))
        lines.append('};')
        reference = budget['reference']
        reference_ptr = 'nullptr'
        if reference is not None:
            uncertainty = reference['uncertainty']
            lines.append('inline constexpr CostMetricUncertainty k_%s_cost_uncertainty[] = {' % pid)
            for metric_id, interval in uncertainty['metrics'].items():
                lines.append('    {%s, %.17g, %.17g, %s},' % (
                    _cpp_string(metric_id), interval['low'], interval['high'], _cpp_string(interval['unit'])))
            lines.append('};')
            lines.append('inline constexpr CostReference k_%s_cost_reference = {%s, %s, 0x%xu, %s, %s, %s, %du, %.17g, k_%s_cost_uncertainty, %d};' % (
                pid, _cpp_string(reference['measurementId']), _cpp_string(reference['build']),
                profile_mask([reference['profile']]), _cpp_string(reference['scene']),
                _cpp_string(json.dumps(reference['config'], sort_keys=True, separators=(',', ':'), ensure_ascii=True)),
                _cpp_string(uncertainty['method']), uncertainty['sampleCount'], uncertainty['confidence'],
                pid, len(uncertainty['metrics'])))
            reference_ptr = '&k_%s_cost_reference' % pid
        budget_state = {'unmeasured': 'kCostBudgetUnmeasured',
                        'relative': 'kCostBudgetRelative',
                        'mixed': 'kCostBudgetMixed'}[budget['state']]
        lines.append('inline constexpr CostBudget k_%s_cost_budget = {%s, %s, k_%s_cost_metrics, %d};' % (
            pid, budget_state, reference_ptr, pid, len(METRIC_IDS)))
    lines.append('inline constexpr PluginRecord kManifest[] = {')
    for plugin in data['plugins']:
        pid = plugin['id'].replace('-', '_')
        recommended = profile_mask(plugin['recommendedProfiles'])
        defaults = profile_mask(plugin['defaultInstallProfiles'])
        required = profile_mask([p for p, kind in plugin['runtimeRequirement'].items() if kind == 'required'])
        partial = profile_mask([p for p, kind in plugin['runtimeRequirement'].items() if kind == 'partial'])
        lines.append('    {%d, %s, %s, %s, %s, 0x%xu, 0x%xu, 0x%xu, 0x%xu, 0x%xu, 0x%xu,' % (
            plugin['index'], _cpp_string(plugin['id']), _cpp_string(plugin['name']),
            _cpp_string(plugin['description']), _cpp_string(plugin['implementationStatus']),
            profile_mask(plugin['profiles']), recommended, defaults,
            profile_mask(plugin['selectionAvailableProfiles']), required, partial))
        lines.append('     k_%s_owned_config_keys, %d, k_%s_hook_points, %d,' %
                     (pid, len(plugin['ownedConfigKeys']), pid, len(plugin['hookPoints'])))
        lines.append('     k_%s_claims, %d, k_%s_dependencies, %d,' %
                     (pid, len(plugin['claims']), pid, len(plugin['dependencies'])))
        lines.append('     k_%s_rigs, %d, %s, &k_%s_cost_budget},' % (
            pid, len(plugin['rigs']), _cpp_string(plugin['costNote']), pid))
    index_names = [plugin['id'].replace('-', '_').title().replace('_', '') for plugin in data['plugins']]
    lines.append('};')
    lines.append('enum PluginIndex : std::uint32_t {')
    lines.extend('    kPlugin%s = %d,' % (index_names[i], i) for i in range(len(index_names)))
    lines.append('    kPluginIndexCount = %d' % len(index_names))
    lines.extend(['};', 'inline constexpr std::uint32_t kPluginCount = %d;' % len(data['plugins']),
                  'inline constexpr std::uint32_t kVrProfileBit = 0x1u;',
                  'inline constexpr std::uint32_t kFlatProfileBit = 0x2u;',
                  '}}  // namespace edvr::plugins', ''])
    return '\n'.join(lines)


def _write_or_report(path, content, dry_run):
    if dry_run:
        print('[edvr] dry run: %s (%d bytes) not written' % (path, len(content.encode('utf-8'))))
        return
    parent = os.path.dirname(os.path.abspath(path))
    os.makedirs(parent, exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)
    print('[edvr] wrote %s' % path)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if '--self-test' in argv:
        return self_test()
    emit_cpp = None
    emit_json = None
    dry_run = '--dry-run' in argv
    for i, arg in enumerate(argv):
        if arg == '--emit-cpp' and i + 1 < len(argv):
            emit_cpp = argv[i + 1]
        elif arg == '--emit-json' and i + 1 < len(argv):
            emit_json = argv[i + 1]
    if any(arg in ('--emit-cpp', '--emit-json') for arg in argv) and not (emit_cpp or emit_json):
        print('plugin_catalog: --emit-cpp and --emit-json require a path')
        return 2
    try:
        data = load_json()
        problems = validate_manifest(data, ini_documented_keys())
    except (OSError, ValueError, TypeError) as exc:
        print('plugin_catalog: cannot load catalog: %s' % exc)
        return 1
    if problems:
        print('plugin_catalog: validation FAILED')
        for problem in problems:
            print('  ' + problem)
        return 1
    if emit_cpp:
        _write_or_report(emit_cpp, render_cpp(data), dry_run)
    if emit_json:
        _write_or_report(emit_json, json.dumps(data, indent=2, ensure_ascii=False) + '\n', dry_run)
    if not emit_cpp and not emit_json:
        print('[edvr] plugin catalog ok: %d plugins, %d owned config keys' %
              (len(data['plugins']), len(ini_documented_keys())))
    return 0


def self_test():
    failures = []

    def check(name, condition):
        if not condition:
            failures.append(name)

    try:
        original = load_json()
        documented = ini_documented_keys()
        check('canonical manifest validates', not validate_manifest(original, documented))
        check('all nine plugin cost budgets are explicitly unmeasured',
              len(original['plugins']) == 9 and all(
                  plugin['costBudget']['state'] == 'unmeasured' and
                  plugin['costBudget']['reference'] is None and
                  all(metric['state'] == 'unmeasured' and
                      set(metric) == {'state', 'unit', 'coverage'}
                      for metric in plugin['costBudget']['metrics'].values())
                  for plugin in original['plugins']))

        bad = copy.deepcopy(original)
        bad['plugins'][0]['costBudget']['metrics']['cpu']['referenceValue'] = 0
        check('unmeasured metric rejects fabricated numeric values',
              any('unmeasured entries cannot carry values' in p
                  for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][0]['costBudget']['state'] = 'relative'
        bad['plugins'][0]['costBudget']['metrics']['cpu'].update(
            {'state': 'relative', 'referenceValue': 1.0, 'maxRelativeIncrease': 0.1})
        check('measured metric rejects an absent reference record',
              any('reference must define measurementId' in p
                  for p in validate_manifest(bad, documented)))

        measured = copy.deepcopy(original)
        budget = measured['plugins'][1]['costBudget']
        budget['state'] = 'relative'
        values = {'cpu': 1.25, 'issuedGpuWork': 3.0, 'directGpu': 0.75}
        intervals = {}
        for metric_id, value in values.items():
            metric = budget['metrics'][metric_id]
            metric.update({'state': 'relative', 'referenceValue': value,
                           'maxRelativeIncrease': 0.1})
            intervals[metric_id] = {'low': value * 0.9, 'high': value * 1.1,
                                    'unit': metric['unit']}
        budget['reference'] = {
            'measurementId': 'self-test.synthetic.reference',
            'build': 'v0.18.0-3-gd57600de-dirty',
            'profile': 'vr',
            'scene': 'synthetic validator fixture',
            'config': {'advanced.synthetic_fixture': True},
            'uncertainty': {'method': 'synthetic shape fixture', 'sampleCount': 3,
                            'confidence': 0.95, 'metrics': intervals},
        }
        check('complete relative schema accepts internally consistent provenance fixture',
              not validate_manifest(measured, documented))
        log_build = copy.deepcopy(measured)
        log_build['plugins'][1]['costBudget']['reference']['build'] = '0.14.1-93-gf78eba4'
        check('relative reference accepts the version identity format used by flight logs',
              not validate_manifest(log_build, documented))
        prerelease_build = copy.deepcopy(measured)
        prerelease_build['plugins'][1]['costBudget']['reference']['build'] = 'v0.18.0-rc.5-26-g5ec0de01'
        check('relative reference accepts logged prerelease git-describe identity',
              not validate_manifest(prerelease_build, documented))
        hash_build = copy.deepcopy(measured)
        hash_build['plugins'][1]['costBudget']['reference']['build'] = 'd57600de'
        check('relative reference accepts git-describe hash-only identity',
              not validate_manifest(hash_build, documented))
        noisy = copy.deepcopy(measured)
        noisy_budget = noisy['plugins'][1]['costBudget']
        noisy_budget['metrics']['cpu']['referenceValue'] = 1.0
        noisy_budget['metrics']['cpu']['maxRelativeIncrease'] = 0.50
        noisy_budget['reference']['uncertainty']['metrics']['cpu'] = {
            'low': 0.95, 'high': 1.05, 'unit': noisy_budget['metrics']['cpu']['unit']}
        check('relative allowance cannot exceed measured one-sided uncertainty margin',
              'cockpit-visuals.costBudget.metrics.cpu.maxRelativeIncrease exceeds the reference uncertainty upper margin'
              in validate_manifest(noisy, documented))
        overflow_margin = copy.deepcopy(measured)
        overflow_metric = overflow_margin['plugins'][1]['costBudget']['metrics']['cpu']
        overflow_metric.update({'referenceValue': 1e-308, 'maxRelativeIncrease': 0.1})
        overflow_margin['plugins'][1]['costBudget']['reference']['uncertainty']['metrics']['cpu'] = {
            'low': 1e-308, 'high': 1e308, 'unit': overflow_metric['unit']}
        check('relative uncertainty arithmetic rejects a non-finite margin',
              any('non-finite relative uncertainty margin' in p
                  for p in validate_manifest(overflow_margin, documented)))
        bad = copy.deepcopy(measured)
        bad['plugins'][1]['costBudget']['reference']['build'] = 'measurement build alpha'
        check('relative reference requires a logged git-describe build identity',
              any('logged git-describe version identity' in p
                  for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(measured)
        bad['plugins'][1]['costBudget']['reference']['build'] = '1-made-up'
        check('relative reference rejects non-production tag prose',
              any('logged git-describe version identity' in p
                  for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(measured)
        bad['plugins'][1]['costBudget']['reference']['uncertainty']['sampleCount'] = 0x100000000
        check('relative reference sample count must fit generated uint32',
              any('sampleCount must fit uint32' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(measured)
        bad['plugins'][1]['costBudget']['metrics']['cpu']['referenceValue'] = 10 ** 10000
        check('oversized JSON integer is rejected without numeric conversion failure',
              any('referenceValue must be a finite positive' in p
                  for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(measured)
        bad['plugins'][1]['costBudget']['reference']['profile'] = 'flat'
        check('relative reference rejects unsupported profile',
              any('reference.profile is unsupported' in p
                  for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(measured)
        del bad['plugins'][1]['costBudget']['reference']['uncertainty']
        check('relative reference rejects missing uncertainty',
              any('reference must define measurementId' in p
                  for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(measured)
        bad['plugins'][1]['costBudget']['reference']['uncertainty']['metrics']['cpu']['low'] = 2.0
        check('relative reference rejects an interval inconsistent with its baseline',
              any('finite ordered interval containing' in p
                  for p in validate_manifest(bad, documented)))

        bad = copy.deepcopy(original)
        bad['plugins'][1]['id'] = bad['plugins'][0]['id']
        check('duplicate plugin ID rejected', any('duplicate plugin id' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][0]['ownedConfigKeys'][0] = 'fix.no_such_setting'
        check('unknown key reference rejected', any('unknown key' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][1]['ownedConfigKeys'].append(bad['plugins'][0]['ownedConfigKeys'][0])
        check('duplicate key ownership rejected', any('duplicate config ownership' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][0]['dependencies'].append({'id': 'ghost', 'kind': 'hard', 'reason': 'fixture'})
        check('unknown dependency rejected', any('unknown plugin' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][0]['dependencies'].append({'id': 'cockpit-visuals', 'kind': 'hard', 'reason': 'fixture'})
        bad['plugins'][1]['dependencies'].append({'id': 'temporal-aa', 'kind': 'soft', 'reason': 'fixture'})
        check('dependency cycle rejected', any('dependency cycle' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][1]['conflicts'].append({'id': 'cockpit-visuals', 'kind': 'hard'})
        check('self conflict rejected', any('cannot conflict with itself' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][1]['conflicts'] = [
            {'id': 'temporal-aa', 'kind': 'soft'}, {'id': 'temporal-aa', 'kind': 'hard'}]
        check('duplicate conflict rejected', any('duplicate conflict' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][5]['dependencies'][0]['kind'] = 'hard'
        bad['plugins'][5]['selectionAvailableProfiles'] = ['vr']
        check('hard dependency availability checked for selectable profiles',
              any('hard dependency temporal-aa is unavailable in a selectable profile' in p
                  for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][0]['defaultInstallProfiles'] = ['bogus']
        check('invalid default profile rejected', any('unsupported profile' in p or 'unknown profile' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['coreOwnedConfigKeys'].pop()
        check('missing ownership rejected', any('has no owner' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['schemaVersion'] = True
        check('boolean schema version rejected', any('schemaVersion' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][1]['claims'][0]['drawShape']['kind'] = 'Z'
        check('unknown draw kind rejected', any('drawShape.kind' in p for p in validate_manifest(bad, documented)))
        bad = copy.deepcopy(original)
        bad['plugins'][1]['claims'][0]['drawShape']['count'] = True
        check('boolean draw count rejected', any('drawShape.count' in p for p in validate_manifest(bad, documented)))

        with tempfile.TemporaryDirectory(prefix='edvr-plugin-catalog-') as td:
            target = os.path.join(td, 'missing', 'manifest.inc')
            content = render_cpp(original)
            _write_or_report(target, content, True)
            check('dry run creates no directory', not os.path.exists(os.path.dirname(target)))
        check('generated metadata has stable indices', '0, "temporal-aa"' in content and
                  '8, "diagnostics"' in content and 'kPluginIndexCount = 9' in content and
                  '0xFCF7BD2896751D96ull' in content and
                  'kCoreOwnedConfigKeyCount' in content and
                  'kPluginTemporalAa = 0' in content and
                  'kClaimCockpitVisualsNightVision = 0' in content and
                  '{0x58u, 240u, 1u}' in content and
                  'struct CostBudget' in content and
                  'k_temporal_aa_cost_budget = {kCostBudgetUnmeasured, nullptr' in content and
                  '"directGpu", kCostMetricUnmeasured' in content and
                  'kCostMetricUnmeasured' in content)
    except Exception as exc:
        failures.append('unexpected exception: %s' % exc)

    if failures:
        print('plugin_catalog: self-test FAILED')
        for failure in failures:
            print('  ' + failure)
        return 1
    print('plugin_catalog: self-test OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
