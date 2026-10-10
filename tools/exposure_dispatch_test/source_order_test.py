"""Source-order gate for the Exposure dispatch observer extraction.

The C++ fixture calls the production observer with fake services. This gate
holds the host hook and core bridge order that the fixture cannot execute.
Both modes are read-only; --self-test mutates strings in memory.
"""

import argparse
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "src/d3d11/exposure_fix.cpp"
MODULE = ROOT / "src/plugins/exposure/exposure_dispatch.cpp"
HEADER = ROOT / "src/plugins/exposure/exposure_dispatch.h"
ACTION = ROOT / "src/plugins/exposure/exposure_actions.cpp"
ACTION_HEADER = ROOT / "src/plugins/exposure/exposure_actions.h"


def code_only(source):
    """Mask comments and quoted text, retaining offsets and line breaks."""
    out = list(source)
    state = "code"
    i = 0
    while i < len(source):
        c = source[i]
        next_c = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if c == "/" and next_c == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "line"
                continue
            if c == "/" and next_c == "*":
                out[i] = out[i + 1] = " "
                i += 2
                state = "block"
                continue
            if c in ('"', "'"):
                out[i] = " "
                state = c
        elif state == "line":
            if c == "\n":
                state = "code"
            else:
                out[i] = " "
        elif state == "block":
            if c == "*" and next_c == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "code"
                continue
            if c != "\n":
                out[i] = " "
        else:
            if c == "\\" and next_c:
                out[i] = " "
                if next_c != "\n":
                    out[i + 1] = " "
                i += 2
                continue
            if c == state:
                state = "code"
            if c != "\n":
                out[i] = " "
        i += 1
    return "".join(out)


def brace_end(code, opening):
    if opening < 0 or code[opening] != "{":
        raise AssertionError("missing block opener")
    depth = 0
    for i in range(opening, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise AssertionError("unclosed block")


def function(code, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", code)
    if not match:
        raise AssertionError("missing function " + name)
    opening = code.find("{", match.start(), match.end())
    closing = brace_end(code, opening)
    return code[opening + 1:closing]


def block(body, pattern):
    match = re.search(pattern, body)
    if not match:
        raise AssertionError("missing block " + pattern)
    opening = body.find("{", match.end())
    if opening < 0:
        raise AssertionError("missing block opener " + pattern)
    closing = brace_end(body, opening)
    return match.start(), closing + 1, body[opening + 1:closing]


def occurrences(code, name):
    return list(re.finditer(r"\b" + re.escape(name) + r"\s*\(", code))


def ordered(body, *parts):
    at = 0
    for part in parts:
        match = re.search(part, body[at:])
        if not match:
            return False
        at += match.end()
    return True


def pins(core_source, module_source, header_source, action_source, action_header_source):
    failures = set()

    def require(condition, label):
        if not condition:
            failures.add(label)

    core = code_only(core_source)
    module = code_only(module_source)
    header = code_only(header_source)
    action = code_only(action_source)
    action_header = code_only(action_header_source)
    try:
        hook = function(core, "hookedDispatch")
        flat = block(hook, r"\bif\s*\(\s*runtimeFlatProfile\s*\(\s*\)\s*\)")
        world = block(hook, r"\bif\s*\(\s*g_vrWorldInternal\s*\)")
        foreign = block(hook, r"\bif\s*\(\s*foreignContext\s*\(\s*self\s*\)\s*\)")
        skip = block(hook, r"\bif\s*\(\s*s->dispatchSkipCount\s*\)")
        guards = []
        guard_pattern = r"\bguardedBudget\s*\(\s*g_budget\s*,\s*\[\s*&\s*\]\s*\{"
        for match in re.finditer(guard_pattern, hook):
            opening = hook.rfind("{", match.start(), match.end())
            closing = brace_end(hook, opening)
            guards.append((match.start(), closing + 1, hook[opening + 1:closing]))
        require(len(guards) == 2, "guard-count")
        require(all(re.match(r"\s*\)\s*;", hook[g[1]:]) for g in guards), "guard-shape")

        begin_calls = occurrences(hook, "exposurePluginBeginDispatch")
        complete_calls = occurrences(hook, "exposurePluginCompleteDispatch")
        real_calls = occurrences(hook, "realDispatch")
        require(len(begin_calls) == 1 and len(complete_calls) == 1, "observer-call-count")
        require(len(real_calls) == 4, "forward-count")

        bypasses = (
            (flat, r"\bg_state->realDispatch\s*\(\s*self\s*,\s*x\s*,\s*y\s*,\s*z\s*\)\s*;", "flat"),
            (world, r"\bg_state->realDispatch\s*\(\s*self\s*,\s*x\s*,\s*y\s*,\s*z\s*\)\s*;", "world"),
            (foreign, r"\bs->realDispatch\s*\(\s*self\s*,\s*x\s*,\s*y\s*,\s*z\s*\)\s*;", "foreign"),
        )
        for (start, end, body), forward, label in bypasses:
            forwards = list(re.finditer(forward, body))
            returns = list(re.finditer(r"\breturn\s*;", body))
            require(len(forwards) == 1 and len(returns) == 1 and
                    returns[0].start() > forwards[0].end(), label + "-return")
            require(not occurrences(body, "exposurePluginBeginDispatch") and
                    not occurrences(body, "exposurePluginCompleteDispatch"), label + "-excluded")
        require(flat[1] < world[0] < world[1] < foreign[0] < foreign[1] < skip[0], "bypass-order")

        skip_hit = block(skip[2], r"\bif\s*\(\s*s->dispatchSkipOcc\s*\[")
        require(ordered(skip_hit[2], r"\+\+s->dispatchSkipped\s*;", r"\bs->computeThisFrame\s*=\s*true\s*;", r"\breturn\s*;"),
                "skip-hit-return")
        require(len(re.findall(r"\breturn\s*;", skip[2])) == 1, "skip-hit-return")
        require(ordered(hook[foreign[1]:skip[0]],
                        r"drawCensusDispatch\s*\(\s*self\s*,\s*x\s*,\s*y\s*,\s*z\s*,\s*false",
                        r"objectProbeNoteDispatch\s*\(\s*self\s*,\s*x\s*,\s*y\s*,\s*z\s*,\s*false",
                        r"engineVelocityNoteChainDispatch\s*\(\s*self\s*,\s*x\s*\)"), "pre-observer-order")

        ticket = re.search(r"\bExposureDispatchTicket\s+ticket\s*\{\s*\}\s*;", hook)
        require(bool(ticket) and skip[1] < ticket.start(), "skip-before-ticket")
        if len(guards) == 2 and len(begin_calls) == 1 and len(complete_calls) == 1:
            first, second = guards
            require(skip[1] < first[0] and first[0] < begin_calls[0].start() < first[1] and
                    bool(re.search(r"\bticket\s*=\s*exposurePluginBeginDispatch\s*\(", first[2])), "begin-guard")
            require(second[0] < complete_calls[0].start() < second[1] and
                    bool(re.search(r"\bexposurePluginCompleteDispatch\s*\(", second[2])), "complete-guard")
            require(not occurrences(first[2], "realDispatch") and
                    not occurrences(second[2], "realDispatch"), "forward-outside-guards")
            require(ordered(hook[first[1]:second[0]],
                            r"\bs->computeThisFrame\s*=\s*true\s*;",
                            r"\bs->realDispatch\s*\(\s*self\s*,\s*x\s*,\s*y\s*,\s*z\s*\)\s*;",
                            r"\bif\s*\(\s*!ticket\.target\s*\)\s*return\s*;"), "forward-between-guards")
            require(first[1] < second[0], "guard-order")

        bridge = function(core, "exposureDispatchApplyPair")
        require(
                bool(re.search(r"\bobserver\s*=\s*static_cast\s*<\s*plugins::exposure::ExposureDispatchObserverState\s*\*\s*>\s*\(\s*observerState\s*\)", bridge)) and
                bool(re.search(r"\bState\s*\*\s*s\s*=\s*static_cast\s*<\s*State\s*\*\s*>\s*\(\s*observer\s*\)", bridge)),
                "bridge-state")
        require(ordered(bridge,
                        r"exposurePluginShareExposure\s*\(",
                        r"if\s*\(\s*s->dampK\s*>\s*0\.0f\s*\)",
                        r"exposurePluginDamp\s*\("),
                "bridge-action-order")
        require(bool(re.search(r"exposurePluginShareExposure\s*\(\s*static_cast\s*<\s*plugins::exposure::ExposureActionState\s*\*>\s*\(\s*g_state\s*\)", bridge)),
                "bridge-share-state")
        require(bool(re.search(r"if\s*\(\s*s->dampK\s*>\s*0\.0f\s*\)", bridge)),
                "bridge-damp-condition")
        require(bool(re.search(r"exposurePluginDamp\s*\(\s*static_cast\s*<\s*plugins::exposure::ExposureActionState\s*\*>\s*\(\s*g_state\s*\)", bridge)),
                "bridge-damp-state")

        action_damp = function(action, "exposurePluginDamp")
        action_share = function(action, "exposurePluginShareExposure")
        require(bool(re.search(r"struct\s+ExposureActionState\s*:\s*ExposureDispatchObserverState", action_header)),
                "action-state-inheritance")
        require(bool(re.search(r"DampGetDevice\s*=\s*96.*?DampCopyReadback\s*=\s*97.*?DampMapReadback\s*=\s*98.*?DampUnmapReadback\s*=\s*99.*?DampWriteFirstEye\s*=\s*100.*?DampWriteSecondEye\s*=\s*101", action, re.S)),
                "damp-api-site-ids")
        require(ordered(action_damp,
                        r"DampGetDevice", r"DampCopyReadback", r"DampMapReadback",
                        r"DampUnmapReadback", r"DampWriteFirstEye", r"DampWriteSecondEye"),
                "damp-api-site-order")
        require(bool(re.search(r"copyCompatible\s*\(\s*a\s*,\s*b\s*\)", action_share) and
                re.search(r"if\s*\(\s*s->copyBtoA\s*\)\s*ctx->CopyResource\s*\(\s*a\s*,\s*b\s*\)\s*;\s*else\s*ctx->CopyResource\s*\(\s*b\s*,\s*a\s*\)", action_share)),
                "share-resource-compatibility-and-direction")

        frame = function(core, "exposureFixFrameBoundary")
        reset = occurrences(frame, "exposurePluginResetDispatchFrame")
        expire = occurrences(frame, "exposurePluginExpireDispatchVerdicts")
        require(len(reset) == 1 and len(expire) == 1, "frame-call-count")
        require(ordered(frame,
                        r"exposurePluginResetDispatchFrame\s*\(\s*static_cast\s*<\s*plugins::exposure::ExposureDispatchObserverState\s*\*\s*>\s*\(\s*s\s*\)\s*\)\s*;",
                        r"\bs->dispatchOccSeen\s*\[\s*i\s*\]\s*=\s*0\s*;",
                        r"exposurePluginExpireDispatchVerdicts\s*\(\s*static_cast\s*<\s*plugins::exposure::ExposureDispatchObserverState\s*\*\s*>\s*\(\s*s\s*\)\s*\)\s*;"),
                "frame-order")

        module_complete = function(module, "exposurePluginCompleteDispatch")
        require(bool(re.search(r"\bif\s*\(\s*!state\s*\|\|\s*!ticket\.target\s*\)\s*return\s*;", module_complete)) and
                len(occurrences(module_complete, "exposureDispatchApplyPair")) == 1,
                "ticket-consumption")
        require(not re.search(r"\b(?:AddRef|Release)\s*\(", module + header), "borrowed-pointers")
        require(bool(re.search(r"\bstruct\s+ExposureDispatchTicket\s*\{\s*uint32_t\s+target\s*;\s*\}", header)),
                "ticket-layout")
    except AssertionError as error:
        failures.add("parse: " + str(error))
    return failures


def replace_once(source, old, new):
    count = source.count(old)
    if count != 1:
        raise AssertionError("mutation anchor count %d: %r" % (count, old[:90]))
    return source.replace(old, new, 1)


def self_test(core, module, header, action, action_header):
    mutations = [
        ("flat-return", "core", "        g_state->realDispatch(self, x, y, z);\n        return;\n",
         "        g_state->realDispatch(self, x, y, z);\n"),
        ("world-return", "core", "if (g_vrWorldInternal) { g_state->realDispatch(self, x, y, z); return; }",
         "if (g_vrWorldInternal) { g_state->realDispatch(self, x, y, z); }"),
        ("foreign-return", "core", "        s->realDispatch(self, x, y, z);\n        return;\n",
         "        s->realDispatch(self, x, y, z);\n"),
        ("skip-hit-return", "core", "                return;\n            }\n            break;",
         "            }\n            break;"),
        ("begin-guard", "core",
         "    guardedBudget(g_budget, [&] {\n        ticket = exposurePluginBeginDispatch(\n            static_cast<plugins::exposure::ExposureDispatchObserverState*>(s));\n    });",
         "    guardedBudget(g_budget, [&] {});\n    ticket = exposurePluginBeginDispatch(\n        static_cast<plugins::exposure::ExposureDispatchObserverState*>(s));"),
        ("forward-outside-guards", "core", "        ticket = exposurePluginBeginDispatch(\n",
         "        s->realDispatch(self, x, y, z);\n        ticket = exposurePluginBeginDispatch(\n"),
        ("frame-call-count", "core", "    exposurePluginResetDispatchFrame(\n",
         "    exposurePluginResetDispatchFrame(static_cast<plugins::exposure::ExposureDispatchObserverState*>(s));\n    exposurePluginResetDispatchFrame(\n"),
        ("frame-call-count", "core", "    exposurePluginExpireDispatchVerdicts(\n",
         "    exposurePluginExpireDispatchVerdicts(static_cast<plugins::exposure::ExposureDispatchObserverState*>(s));\n    exposurePluginExpireDispatchVerdicts(\n"),
        ("bridge-state", "core", "    State* s = static_cast<State*>(observer);",
         "    State* s = g_state;"),
        ("bridge-share-state", "core",
         "        static_cast<plugins::exposure::ExposureActionState*>(g_state), context,\n        first, second);",
         "        static_cast<plugins::exposure::ExposureActionState*>(s), context,\n        first, second);"),
        ("bridge-action-order", "core",
         "    plugins::exposure::exposurePluginShareExposure(\n"
         "        static_cast<plugins::exposure::ExposureActionState*>(g_state), context,\n"
         "        first, second);\n"
         "    if (s->dampK > 0.0f) {\n"
         "        plugins::exposure::exposurePluginDamp(\n"
         "            static_cast<plugins::exposure::ExposureActionState*>(g_state),\n"
         "            context, first[1]);\n"
         "    }",
         "    if (s->dampK > 0.0f) {\n"
         "        plugins::exposure::exposurePluginDamp(\n"
         "            static_cast<plugins::exposure::ExposureActionState*>(g_state),\n"
         "            context, first[1]);\n"
         "    }\n"
         "    plugins::exposure::exposurePluginShareExposure(\n"
         "        static_cast<plugins::exposure::ExposureActionState*>(g_state), context,\n"
         "        first, second);"),
        ("bridge-damp-state", "core",
         "            static_cast<plugins::exposure::ExposureActionState*>(g_state),\n            context, first[1]);",
         "            static_cast<plugins::exposure::ExposureActionState*>(s),\n            context, first[1]);"),
        ("bridge-damp-condition", "core", "        first, second);\n    if (s->dampK > 0.0f) {\n",
         "        first, second);\n    if (g_state->dampK > 0.0f) {\n"),
        ("damp-api-site-ids", "action", "    DampGetDevice = 96,",
         "    DampGetDevice = 95,"),
        ("borrowed-pointers", "module", "    ++s->seenThisFrame;",
         "    context->AddRef();\n    ++s->seenThisFrame;"),
    ]
    for label, target, old, new in mutations:
        changed = replace_once({"core": core, "module": module, "action": action}[target], old, new)
        failures = pins(changed if target == "core" else core,
                        changed if target == "module" else module, header,
                        changed if target == "action" else action, action_header)
        if label not in failures:
            raise AssertionError("mutation escaped %s: %s" % (label, sorted(failures)))

    # Move the complete Begin region under the skip opener, after its condition
    # but before the skip-hit return. The test must read the entire skip block.
    begin = ("    ExposureDispatchTicket ticket{};\n    guardedBudget(g_budget, [&] {\n"
             "        ticket = exposurePluginBeginDispatch(\n"
             "            static_cast<plugins::exposure::ExposureDispatchObserverState*>(s));\n    });\n")
    moved = replace_once(core, begin, "")
    moved = replace_once(moved, "    if (s->dispatchSkipCount) {\n",
                         "    if (s->dispatchSkipCount) {\n" + begin)
    if "skip-before-ticket" not in pins(moved, module, header, action, action_header):
        raise AssertionError("Begin inside the whole skip block escaped")

    # Add a forward inside the Complete guard as well as the Begin guard above.
    complete = "        exposurePluginCompleteDispatch(\n"
    changed = replace_once(core, complete, "        s->realDispatch(self, x, y, z);\n" + complete)
    if "forward-outside-guards" not in pins(changed, module, header, action, action_header):
        raise AssertionError("forward inside Complete guard escaped")

    # Rearranging frame calls around skip-counter clearing must fail even
    # though each call is still present exactly once.
    reset = ("    exposurePluginResetDispatchFrame(\n"
             "        static_cast<plugins::exposure::ExposureDispatchObserverState*>(s));\n")
    moved = replace_once(core, reset, "")
    moved = replace_once(moved, "    for (uint32_t i = 0; i < 4; ++i) s->dispatchOccSeen[i] = 0;\n",
                         "    for (uint32_t i = 0; i < 4; ++i) s->dispatchOccSeen[i] = 0;\n" + reset)
    if "frame-order" not in pins(moved, module, header, action, action_header):
        raise AssertionError("reset after skip-counter clear escaped")
    expire = ("    exposurePluginExpireDispatchVerdicts(\n"
              "        static_cast<plugins::exposure::ExposureDispatchObserverState*>(s));\n")
    moved = replace_once(core, expire, "")
    moved = replace_once(moved, reset, reset + expire)
    if "frame-order" not in pins(moved, module, header, action, action_header):
        raise AssertionError("expiry before skip-counter clear escaped")
    return len(mutations) + 4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true", help="check source pins without writing")
    mode.add_argument("--self-test", action="store_true", help="also run in-memory mutations")
    args = parser.parse_args()
    core, module, header, action, action_header = (
        p.read_text(encoding="utf-8") for p in (CORE, MODULE, HEADER, ACTION, ACTION_HEADER))
    failures = pins(core, module, header, action, action_header)
    if failures:
        for failure in sorted(failures):
            print("FAIL:", failure)
        return 1
    if args.self_test:
        count = self_test(core, module, header, action, action_header)
        print("PASS: exposure dispatch source order (%d in-memory mutations; no writes)" % count)
    else:
        print("PASS: exposure dispatch source order (dry run; no writes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
