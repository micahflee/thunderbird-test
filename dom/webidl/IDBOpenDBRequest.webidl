/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://w3c.github.io/IndexedDB/#idbopendbrequest
 */

[Exposed=(Window,Worker), Func="IDBFactory::IsEnabled"]
interface IDBOpenDBRequest : IDBRequest {
                attribute EventHandler onblocked;

                attribute EventHandler onupgradeneeded;
};
