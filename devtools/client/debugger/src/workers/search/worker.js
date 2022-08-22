/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import getMatches from "./get-matches";
import { findSourceMatches } from "./project-search";
import { workerHandler } from "devtools/client/shared/worker-utils";

self.onmessage = workerHandler({ getMatches, findSourceMatches });
