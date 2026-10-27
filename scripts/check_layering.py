#!/usr/bin/env python3
# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT
"""Layering guard: enforce the dependency table of the QarOS integration code.

Usage:
    python scripts/check_layering.py --module <name> [--repo-root <dir>]
    python scripts/check_layering.py --all [--repo-root <dir>]
    python scripts/check_layering.py --self-test

--all checks every module of the table whose folder exists under the repository root: a repository holds
only some of the modules, and the layering table describes the whole family. It exits 2 when no module
folder exists. --module <name> exits 2 when the folder of that module is absent.

Rules (comments and string literals are ignored; every #include line is classified):

  L1  include classes   each module may include only the foreign header families listed in POLICY
                        (standard library, Qt, Ogre, ROS 2, RViz, platform headers, GoogleTest, stb);
  L2  project edges     each module may include only the project modules listed in POLICY, and some
                        edges only through the named headers (EDGE_ONLY): no upward or sideways edge;
                        every example folder under examples/ has its own entry in EXAMPLE_POLICY
                        (foreign families, project modules and the adapter components it may include);
  L3  vendored header   only qar_ar_core/include/qar_ar_core/qar_streaming_c.hpp includes qar_streaming.h;
  L4  core calls no API qar_ar_core production code calls no qar_* C function (it uses the C API's types
                        only, so it never needs the SDK DLL or the dynamic-loading tables);
  L5  adapter parts     every qar_qar_adapter file belongs to one component (ADAPTER_COMPONENT) and includes
                        adapter headers of its own component and of the components it may use only;
  L6  loader            QAR_IMPLEMENT_DYNAMIC_LOADING() appears exactly once, in qar_capi_loader.cpp;
                        nothing #defines QAR_ENABLE_DYNAMIC_LOADING (CMake does), and only the wrapper
                        #defines QAR_ENABLE_OPENGL.

Exit status 0 = clean, 1 = violations (listed as path:line: [rule] detail), 2 = usage error.
Standard library only (Python 3.8+).
"""
import argparse
import os
import re
import shutil
import sys
import tempfile

MODULES = ['qar_ar_core', 'qar_ogre_streaming', 'qar_qar_adapter', 'examples', 'qar_qt_streaming',
           'qar_ui_model', 'qar_web_panels', 'qar_ros_ui', 'qar_rviz_streaming']

# Include roots of the project: the first path component of a project include.
PROJECT_ROOTS = {
    'qar_ar_core': 'qar_ar_core',
    'qar_ogre_streaming': 'qar_ogre_streaming',
    'qar_ogre_streaming_test_support': 'qar_ogre_streaming',
    'qar_qar_adapter': 'qar_qar_adapter',
    'qar_qt_streaming': 'qar_qt_streaming',
    'qar_ui_model': 'qar_ui_model',
    'qar_web_panels': 'qar_web_panels',
    'qar_ros_ui': 'qar_ros_ui',
    'qar_rviz_streaming': 'qar_rviz_streaming',
}

# module -> allowed project modules, foreign classes in production code, extra classes in tests,
# allowed platform headers.
POLICY = {
    'qar_ar_core': dict(project={'qar_ar_core'}, prod={'STD', 'VENDORED', 'PLATFORM'}, test={'GTEST'},
                        platform={'share.h'}),
    'qar_ogre_streaming': dict(project={'qar_ogre_streaming', 'qar_ar_core'}, prod={'STD', 'OGRE', 'PLATFORM'},
                               test={'GTEST'}, platform={'windows.h'}),
    'qar_qar_adapter': dict(project={'qar_qar_adapter', 'qar_ar_core', 'qar_ogre_streaming'},
                            prod={'STD', 'PLATFORM'}, test={'GTEST'}, platform={'windows.h'}),
    'examples': dict(project={'qar_ogre_streaming', 'qar_ar_core'}, prod={'STD', 'OGRE', 'STB'}, test=set(),
                     platform=set()),
    'qar_qt_streaming': dict(project={'qar_qt_streaming', 'qar_ar_core'}, prod={'STD', 'QT'}, test={'GTEST'},
                             platform=set()),
    'qar_ui_model': dict(project={'qar_ui_model'}, prod={'STD'}, test={'GTEST'}, platform=set()),
    'qar_web_panels': dict(project={'qar_web_panels', 'qar_ui_model', 'qar_ar_core'}, prod={'STD', 'QT'},
                           test={'GTEST'}, platform=set()),
    'qar_ros_ui': dict(project={'qar_ros_ui', 'qar_ui_model'}, prod={'STD', 'ROS'}, test={'GTEST', 'PLATFORM'},
                       platform={'process.h', 'unistd.h'}),
    'qar_rviz_streaming': dict(project=set(PROJECT_ROOTS.values()),
                               prod={'STD', 'QT', 'OGRE', 'ROS', 'RVIZ', 'PLATFORM', 'STB'},
                               test={'GTEST'}, platform={'windows.h', 'mmsystem.h', 'process.h', 'unistd.h'}),
}

# Policy of each example folder under examples/ (files elsewhere under examples/ use POLICY['examples']):
# the same keys as POLICY, and 'adapter': the qar_qar_adapter components whose headers the example may include.
EXAMPLE_POLICY = {
    'ogre_standalone_streaming_sample': dict(project={'qar_ogre_streaming', 'qar_ar_core'},
                                             prod={'STD', 'OGRE', 'STB'}, test=set(), platform=set(), adapter=set()),
    'qaros_live_stream': dict(project={'qar_ogre_streaming', 'qar_ar_core', 'qar_qar_adapter'},
                              prod={'STD', 'OGRE', 'STB'}, test=set(), platform=set(), adapter={'render', 'session'}),
    'qt_panel_example': dict(project={'qar_qt_streaming', 'qar_ar_core', 'qar_qar_adapter'},
                             prod={'STD', 'QT'}, test=set(), platform=set(), adapter={'session', 'panels'}),
    'web_panel_example': dict(project={'qar_ui_model', 'qar_web_panels', 'qar_ar_core', 'qar_qar_adapter'},
                              prod={'STD', 'QT'}, test=set(), platform=set(), adapter={'render', 'session', 'url'}),
    'ros_topic_panel_example': dict(project={'qar_ui_model', 'qar_web_panels', 'qar_ros_ui', 'qar_ar_core',
                                             'qar_qar_adapter'},
                                    prod={'STD', 'QT', 'ROS'}, test=set(), platform=set(),
                                    adapter={'render', 'session', 'url'}),
}

# (module, target project module) -> the only headers that edge may use.
EDGE_ONLY = {
    ('qar_ogre_streaming', 'qar_ar_core'): {'qar_ar_core/qar_streaming_c.hpp'},
    ('qar_web_panels', 'qar_ar_core'): {'qar_ar_core/qar_streaming_c.hpp'},
    ('qar_qar_adapter', 'qar_ogre_streaming'): {'qar_ogre_streaming/qar_render_sender_adapter.hpp',
                                                'qar_ogre_streaming/types.hpp'},
}

WRAPPER = 'qar_ar_core/include/qar_ar_core/qar_streaming_c.hpp'
LOADER = 'qar_qar_adapter/src/qar_capi_loader.cpp'

# Every qar_qar_adapter file -> its component; and which components' headers each may include.
ADAPTER_COMPONENT = {
    'qar_capi_loader.cpp': 'capi', 'qar_capi_input_slots.hpp': 'capi', 'qar_capi_input_slots.cpp': 'capi',
    'qar_capi_session_owner.hpp': 'capi',
    'log_time.hpp': 'capi', 'panel_state_log.hpp': 'capi', 'frame_stall_probe.hpp': 'capi',
    'gui_call_guard.hpp': 'capi', 'gui_call_guard.cpp': 'capi', 'panel_state_guard.hpp': 'capi',
    'qar_capi_render_sender_adapter.hpp': 'render', 'qar_capi_render_sender_adapter.cpp': 'render',
    'render_timing.hpp': 'render',
    'geo_anchor.hpp': 'render', 'geo_anchor.cpp': 'render',
    'qar_session_owner.hpp': 'session', 'qar_session_owner.cpp': 'session',
    'identity_store.hpp': 'session', 'identity_store.cpp': 'session',
    'onboarding_status.hpp': 'session', 'onboarding_status.cpp': 'session',
    'target_rule.hpp': 'session', 'env_config.hpp': 'session', 'env_config.cpp': 'session',
    'fake_qar_api.hpp': 'test', 'session_owner_harness.hpp': 'test',
    'qar_capi_panel_sender_adapter.hpp': 'panels', 'qar_capi_panel_sender_adapter.cpp': 'panels',
    'qar_capi_panel_input_source.hpp': 'panels', 'qar_capi_panel_input_source.cpp': 'panels',
    'qar_capi_url_panel_adapter.hpp': 'url', 'qar_capi_url_panel_adapter.cpp': 'url',
    'qar_capi_spatial_gesture_source.hpp': 'gestures', 'qar_capi_spatial_gesture_source.cpp': 'gestures',
    'qar_capi_input_internal.hpp': 'input_private',
}
COMPONENT_MAY_INCLUDE = {
    'capi': {'capi'},
    'session': {'capi', 'session'},
    'render': {'capi', 'session', 'render'},
    'panels': {'capi', 'panels', 'input_private'},
    'url': {'capi', 'url'},
    'gestures': {'capi', 'gestures', 'input_private'},
    'input_private': {'capi'},
    'test': {'capi', 'session', 'render', 'panels', 'url', 'test'},
}
# Only these components may use the Ogre library's seam headers (EDGE_ONLY above).
OGRE_SEAM_COMPONENTS = {'render'}

C_HEADERS = {'assert.h', 'ctype.h', 'errno.h', 'float.h', 'limits.h', 'math.h', 'signal.h', 'stdarg.h',
             'stdbool.h', 'stddef.h', 'stdint.h', 'stdio.h', 'stdlib.h', 'string.h', 'time.h', 'wchar.h'}
PLATFORM_HEADERS = {'windows.h', 'mmsystem.h', 'process.h', 'unistd.h', 'share.h', 'dlfcn.h', 'io.h'}
ROS_RX = re.compile(r'^(rcl\w*|rcutils|rcpputils|rmw\w*|rosidl\w*|ament\w*|pluginlib|class_loader|tf2\w*|'
                    r'message_filters|image_transport|\w+_msgs|\w+_srvs|\w+_interfaces)/')
INCLUDE_RX = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^">]+)[">]', re.M)
SOURCE_EXT = ('.hpp', '.h', '.cpp', '.cc', '.cxx', '.hh')
SKIP_DIRS = {'build', 'install', 'log', 'out', '__pycache__', '.git', 'docs'}


def classify(inc, quote, including_file):
    first = inc.split('/')[0]
    if '/' in inc and first in PROJECT_ROOTS:
        return 'PROJECT'
    if inc == 'qar_streaming.h':
        return 'VENDORED'
    if re.match(r'^(gtest|gmock)/', inc):
        return 'GTEST'
    if re.match(r'^Q[A-Za-z]\w*(/|$)', inc) or inc.startswith('Qt'):
        return 'QT'
    if re.match(r'^Ogre\w*\.h$', inc) or re.match(r'^(OGRE|Ogre)/', inc):
        return 'OGRE'
    if re.match(r'^rviz\w*/', inc):
        return 'RVIZ'
    if ROS_RX.match(inc):
        return 'ROS'
    if inc in PLATFORM_HEADERS or inc.startswith('GL/'):
        return 'PLATFORM'
    if re.match(r'^stb_\w+\.h$', inc):
        return 'STB'
    if ('/' not in inc and '.' not in inc) or inc in C_HEADERS:
        return 'STD'
    if quote == '"' and os.path.isfile(os.path.join(os.path.dirname(including_file), inc)):
        return 'LOCAL'
    return 'OTHER'


def mask(code):
    """Blank comments and string/char literals, keeping offsets and newlines."""
    out = list(code)
    i, n = 0, len(code)

    def blank(a, b):
        for k in range(a, min(b, n)):
            if out[k] not in '\r\n':
                out[k] = ' '
    while i < n:
        if code.startswith('//', i):
            j = code.find('\n', i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif code.startswith('/*', i):
            j = code.find('*/', i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif code[i] == '"' or (code[i] == "'" and not (i > 0 and code[i - 1].isalnum())):
            q = code[i]
            j = i + 1
            while j < n and code[j] != q and code[j] != '\n':
                j += 2 if code[j] == '\\' else 1
            blank(i, j + 1)
            i = j + 1
        else:
            i += 1
    return ''.join(out)


def files_of(root, module):
    base = os.path.join(root, module)
    for dp, dn, fn in os.walk(base):
        dn[:] = sorted(d for d in dn if d not in SKIP_DIRS)
        for f in sorted(fn):
            if f.endswith(SOURCE_EXT):
                yield os.path.join(dp, f)


def rel(root, path):
    return os.path.relpath(path, root).replace(os.sep, '/')


def policy_of(module, parts):
    """(policy, name used in messages) of a file: the files of an example folder follow EXAMPLE_POLICY."""
    if module == 'examples' and len(parts) > 2 and parts[1] in EXAMPLE_POLICY:
        return EXAMPLE_POLICY[parts[1]], 'examples/' + parts[1]
    return POLICY[module], module


def check_module(root, module):
    errors = []
    loader_hits = []
    for path in files_of(root, module):
        r = rel(root, path)
        parts = r.split('/')
        pol, label = policy_of(module, parts)
        is_test = len(parts) > 1 and parts[1] in ('test', 'test_support')
        text = open(path, encoding='utf-8', errors='replace').read()
        masked = mask(text)
        allowed = set(pol['prod']) | (set(pol['test']) if is_test else set())
        component = None
        if module == 'qar_qar_adapter':
            component = 'test' if is_test else ADAPTER_COMPONENT.get(parts[-1])
            if component is None:
                errors.append('%s:1: [L5] file belongs to no adapter component: add it to ADAPTER_COMPONENT' % r)
        for m in INCLUDE_RX.finditer(text):
            line = text.count('\n', 0, m.start()) + 1
            quote, inc = m.group(1), m.group(2)
            if masked[m.start():m.end()].strip() == '':      # the #include sits inside a comment
                continue
            kind = classify(inc, quote, path)
            if kind == 'VENDORED':
                if r != WRAPPER:
                    errors.append('%s:%d: [L3] only %s may include <%s>' % (r, line, WRAPPER, inc))
                continue
            if kind == 'PROJECT':
                target = PROJECT_ROOTS[inc.split('/')[0]]
                if target not in pol['project']:
                    errors.append('%s:%d: [L2] %s must not include %s (module %s)' % (r, line, label, inc, target))
                    continue
                only = EDGE_ONLY.get((module, target))
                if only is not None and inc not in only:
                    errors.append('%s:%d: [L2] %s may use %s only through %s, not %s'
                                  % (r, line, module, target, ', '.join(sorted(only)), inc))
                if module == 'examples' and target == 'qar_qar_adapter':
                    example_component = ADAPTER_COMPONENT.get(inc.split('/')[-1])
                    if example_component not in pol['adapter']:
                        errors.append('%s:%d: [L5] %s must not include %s (component %s; allowed: %s)'
                                      % (r, line, label, inc, example_component, ', '.join(sorted(pol['adapter'])) or 'none'))
                if module == 'qar_qar_adapter' and component is not None:
                    if target == 'qar_ogre_streaming' and component not in OGRE_SEAM_COMPONENTS and component != 'test':
                        errors.append('%s:%d: [L5] component %s must not include %s (only render may)'
                                      % (r, line, component, inc))
                    if target == 'qar_qar_adapter':
                        other = ADAPTER_COMPONENT.get(inc.split('/')[-1])
                        if other is None or other not in COMPONENT_MAY_INCLUDE[component]:
                            errors.append('%s:%d: [L5] component %s must not include %s (component %s)'
                                          % (r, line, component, inc, other))
                continue
            if kind == 'LOCAL':
                if module == 'qar_qar_adapter' and component is not None:
                    other = ADAPTER_COMPONENT.get(inc.split('/')[-1])
                    if other is None or other not in COMPONENT_MAY_INCLUDE[component]:
                        errors.append('%s:%d: [L5] component %s must not include "%s" (component %s)'
                                      % (r, line, component, inc, other))
                continue
            if kind == 'PLATFORM':
                if 'PLATFORM' not in allowed or (inc not in pol['platform'] and not is_test):
                    errors.append('%s:%d: [L1] platform header <%s> is not allowed in %s' % (r, line, inc, label))
                continue
            if kind not in allowed:
                errors.append('%s:%d: [L1] %s header <%s> is not allowed in %s%s'
                              % (r, line, kind, inc, label, ' tests' if is_test else ''))
        # L4: qar_ar_core production code calls no QarOS C API function
        if module == 'qar_ar_core' and not is_test:
            for m in re.finditer(r'\bqar_[a-z0-9_]+\s*\(', masked):
                line = masked.count('\n', 0, m.start()) + 1
                errors.append('%s:%d: [L4] qar_ar_core must not call the C API: %s' % (r, line, m.group(0).strip()))
        # L6: one loader, macros defined in one place only
        for m in re.finditer(r'\bQAR_IMPLEMENT_DYNAMIC_LOADING\s*\(\s*\)', masked):
            loader_hits.append(r)
            if r != LOADER:
                line = masked.count('\n', 0, m.start()) + 1
                errors.append('%s:%d: [L6] QAR_IMPLEMENT_DYNAMIC_LOADING() belongs in %s only' % (r, line, LOADER))
        for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+(QAR_ENABLE_DYNAMIC_LOADING|QAR_ENABLE_OPENGL)\b', masked, re.M):
            if m.group(1) == 'QAR_ENABLE_OPENGL' and r == WRAPPER:
                continue
            line = masked.count('\n', 0, m.start()) + 1
            errors.append('%s:%d: [L6] %s is defined by %s, not here'
                          % (r, line, m.group(1), 'CMake (qar_qar_adapter_capi)' if 'DYNAMIC' in m.group(1)
                             else WRAPPER))
    if module == 'qar_qar_adapter' and loader_hits.count(LOADER) != 1:
        errors.append('%s:1: [L6] expected exactly one QAR_IMPLEMENT_DYNAMIC_LOADING() here, found %d'
                      % (LOADER, loader_hits.count(LOADER)))
    return errors


def run(root, modules):
    total = 0
    for module in modules:
        if not os.path.isdir(os.path.join(root, module)):
            print('check_layering: module directory not found: %s' % module)
            return 2
        errs = check_module(root, module)
        n = len(list(files_of(root, module)))
        if errs:
            print('check_layering %s: %d violation(s) in %d files' % (module, len(errs), n))
            for e in errs:
                print('  ' + e)
        else:
            print('check_layering %s: OK (%d files)' % (module, n))
        total += len(errs)
    return 1 if total else 0


def self_test():
    """Build a small tree with one violation per rule and check that each is reported."""
    tmp = tempfile.mkdtemp(prefix='qar_layering_selftest_')
    try:
        def put(path, text):
            p = os.path.join(tmp, path)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'w', encoding='utf-8') as f:
                f.write(text)
        put(WRAPPER, '#define QAR_ENABLE_OPENGL\n#include <qar_streaming.h>\n')
        put('qar_ar_core/src/bad.cpp', '#include <QWidget>\n#include "qar_qt_streaming/x.hpp"\n'
            'int f() { return qar_library_load("x"); }\n// #include <QString> in a comment is fine\n')
        put('qar_qar_adapter/src/qar_capi_loader.cpp', 'QAR_IMPLEMENT_DYNAMIC_LOADING()\n')
        put('qar_qar_adapter/src/qar_capi_url_panel_adapter.cpp',
            '#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"\n#include <qar_streaming.h>\n')
        put('qar_qar_adapter/src/stray.cpp', '#define QAR_ENABLE_DYNAMIC_LOADING\nQAR_IMPLEMENT_DYNAMIC_LOADING()\n')
        put('qar_ui_model/src/u.cpp', '#include <windows.h>\n#include <vector>\n')
        put('qar_web_panels/src/w.cpp', '#include "qar_ar_core/spatial_pick.hpp"\n#include <QTcpServer>\n')
        put('examples/qaros_live_stream/main.cpp',
            '#include <QWidget>\n#include "qar_qt_streaming/x.hpp"\n#include "qar_qar_adapter/qar_capi_panel_sender_adapter.hpp"\n'
            '#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"\n')
        put('examples/qt_panel_example/main.cpp',
            '#include <QWidget>\n#include "qar_qt_streaming/x.hpp"\n#include "qar_qar_adapter/qar_capi_panel_sender_adapter.hpp"\n'
            '#include "qar_qar_adapter/qar_session_owner.hpp"\n#include "qar_qar_adapter/qar_capi_url_panel_adapter.hpp"\n')
        put('examples/stray.cpp', '#include <QWidget>\n')
        expect = {
            'qar_ar_core': ['[L1] QT header <QWidget>', '[L2] qar_ar_core must not include qar_qt_streaming/x.hpp',
                            '[L4] qar_ar_core must not call the C API: qar_library_load('],
            'qar_qar_adapter': ['[L5] component url must not include qar_qar_adapter/qar_capi_render_sender_adapter.hpp',
                                '[L3] only', '[L5] file belongs to no adapter component',
                                '[L6] QAR_ENABLE_DYNAMIC_LOADING is defined by CMake',
                                '[L6] QAR_IMPLEMENT_DYNAMIC_LOADING() belongs in'],
            'qar_ui_model': ['[L1] platform header <windows.h>'],
            'qar_web_panels': ['[L2] qar_web_panels may use qar_ar_core only through'],
            'examples': ['[L1] QT header <QWidget> is not allowed in examples/qaros_live_stream',
                         '[L2] examples/qaros_live_stream must not include qar_qt_streaming/x.hpp',
                         '[L5] examples/qaros_live_stream must not include qar_qar_adapter/qar_capi_panel_sender_adapter.hpp',
                         '[L5] examples/qt_panel_example must not include qar_qar_adapter/qar_capi_url_panel_adapter.hpp',
                         'examples/stray.cpp:1: [L1] QT header <QWidget> is not allowed in examples'],
        }
        failed = 0
        for module, wants in expect.items():
            errs = check_module(tmp, module)
            for w in wants:
                if not any(w in e for e in errs):
                    print('self-test: %s did not report %r; got %s' % (module, w, errs))
                    failed += 1
            if module == 'qar_ar_core' and any('QString' in e for e in errs):
                print('self-test: an #include inside a comment was reported')
                failed += 1
        example_errors = [e for e in check_module(tmp, 'examples') if e.startswith('examples/qt_panel_example/')]
        if len(example_errors) != 1:
            print('self-test: examples/qt_panel_example reported %d violations, expected only the url header: %s'
                  % (len(example_errors), example_errors))
            failed += 1
        clean =[e for e in check_module(tmp, 'qar_ar_core') if e.startswith(WRAPPER)]
        if clean:
            print('self-test: the wrapper itself was reported: %s' % clean)
            failed += 1
        print('check_layering self-test: %s' % ('OK' if not failed else '%d FAILURE(S)' % failed))
        return 1 if failed else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--repo-root', default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--module', choices=MODULES)
    g.add_argument('--all', action='store_true')
    g.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    root = os.path.abspath(a.repo_root)
    if a.all:
        modules = [m for m in MODULES if os.path.isdir(os.path.join(root, m))]
        if not modules:
            print('check_layering: no module directory found under %s' % root)
            return 2
    else:
        modules = [a.module]
    return run(root, modules)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
