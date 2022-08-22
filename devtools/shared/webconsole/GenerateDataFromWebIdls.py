"""
This script parses mozilla-central's WebIDL bindings and writes a JSON-formatted
subset of the function bindings to several files:
- "devtools/server/actors/webconsole/webidl-pure-allowlist.js" (for eager evaluation processing)
- "devtools/server/actors/webconsole/webidl-deprecated-list.js"

Run this script via

> ./mach python devtools/shared/webconsole/GenerateDataFromWebIdls.py

with a mozconfig that references a built non-artifact build.
"""

from __future__ import absolute_import, unicode_literals, print_function
from os import path, remove, system
import json
import WebIDL
import buildconfig

# This is an explicit list of interfaces to load [Pure] and [Constant]
# annotation for. There are a bunch of things that are pure in other interfaces
# that we don't care about in the context of the devtools.
PURE_INTERFACE_ALLOWLIST = set(
    [
        "Document",
        "Node",
        "DOMTokenList",
        "Element",
        "Performance",
        "URLSearchParams",
        "FormData",
        "Headers",
    ]
)

# This is an explicit list of interfaces to exclude.
DEPRECATED_INTERFACE__EXCLUDE_LIST = set(
    [
        "External",
        "TestExampleInterface",
        "TestInterface",
        "TestJSImplInterface",
        "TestingDeprecatedInterface",
    ]
)

FILE_TEMPLATE = """\
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file is automatically generated by the GenerateDataFromWebIdls.py
// script. Do not modify it manually.
"use strict";

module.exports = %(data)s;
"""

pure_output_file = path.join(
    buildconfig.topsrcdir, "devtools/server/actors/webconsole/webidl-pure-allowlist.js"
)
deprecated_output_file = path.join(
    buildconfig.topsrcdir, "devtools/server/actors/webconsole/webidl-deprecated-list.js"
)

input_file = path.join(buildconfig.topobjdir, "dom/bindings/file-lists.json")

if not path.isfile(input_file):
    raise Exception(
        "Script must be run with a mozconfig referencing a non-artifact OBJDIR"
    )

file_list = json.load(open(input_file))

parser = WebIDL.Parser()
for filepath in file_list["webidls"]:
    with open(filepath, "r", encoding="utf8") as f:
        parser.parse(f.read(), filepath)
results = parser.finish()

pure_output = {}
deprecated_output = {}
for result in results:
    if isinstance(result, WebIDL.IDLInterface):
        iface = result.identifier.name

        for member in result.members:
            name = member.identifier.name

            # We only care about methods because eager evaluation assumes that
            # all getter functions are side-effect-free.
            if member.isMethod() and member.affects == "Nothing":
                if (
                    PURE_INTERFACE_ALLOWLIST and not iface in PURE_INTERFACE_ALLOWLIST
                ) or name.startswith("_"):
                    continue
                if not iface in pure_output:
                    pure_output[iface] = []
                if member.isStatic():
                    pure_output[iface].append([name])
                else:
                    pure_output[iface].append(["prototype", name])
            if (
                not iface in DEPRECATED_INTERFACE__EXCLUDE_LIST
                and (member.isMethod() or member.isAttr())
                and member.getExtendedAttribute("Deprecated")
            ):
                if not iface in deprecated_output:
                    deprecated_output[iface] = []
                if member.isStatic():
                    deprecated_output[iface].append([name])
                else:
                    deprecated_output[iface].append(["prototype", name])

with open(pure_output_file, "w") as f:
    f.write(FILE_TEMPLATE % {"data": json.dumps(pure_output, indent=2, sort_keys=True)})
print("Successfully generated", pure_output_file)

with open(deprecated_output_file, "w") as f:
    f.write(
        FILE_TEMPLATE
        % {"data": json.dumps(deprecated_output, indent=2, sort_keys=True)}
    )
print("Successfully generated", deprecated_output_file)

print("Formatting files...")
system("./mach eslint --fix " + pure_output_file + " " + deprecated_output_file)
print("Files are now properly formatted")

# Parsing the idls generate a parser.out file that we don't have any use of.
if path.exists("parser.out"):
    remove("parser.out")
print("DONE")
