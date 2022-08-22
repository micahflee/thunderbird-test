/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * http://www.whatwg.org/specs/web-apps/current-work/multipage/workers.html#the-workerglobalscope-common-interface
 *
 * © Copyright 2004-2011 Apple Computer, Inc., Mozilla Foundation, and Opera
 * Software ASA.
 * You are granted a license to use, reproduce and create derivative works of
 * this document.
 */

[Global=(Worker,DedicatedWorker),
 Exposed=DedicatedWorker]
interface DedicatedWorkerGlobalScope : WorkerGlobalScope {
  [Replaceable]
  readonly attribute DOMString name;

  [Throws]
  void postMessage(any message, sequence<object> transfer);
  [Throws]
  void postMessage(any message, optional StructuredSerializeOptions options = {});

  void close();

  attribute EventHandler onmessage;
  attribute EventHandler onmessageerror;

  // https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#animation-frames
  // Ideally we would just include AnimationFrameProvider to add the interface,
  // but we cannot make an include conditional.
  [Pref="dom.workers.requestAnimationFrame", Throws]
  long requestAnimationFrame(FrameRequestCallback callback);

  [Pref="dom.workers.requestAnimationFrame", Throws]
  void cancelAnimationFrame(long handle);
};
