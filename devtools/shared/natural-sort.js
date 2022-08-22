/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Based on the Natural Sort algorithm for Javascript - Version 0.8.1 - adapted
 * for Firefox DevTools and released under the MIT license.
 *
 * Author: Jim Palmer (based on chunking idea from Dave Koelle)
 *
 * Repository:
 *   https://github.com/overset/javascript-natural-sort/
 */

"use strict";

const tokenizeNumbersRx = /(^([+\-]?\d+(?:\.\d*)?(?:[eE][+\-]?\d+)?(?=\D|\s|$))|^0x[\da-fA-F]+$|\d+)/g;
const dateRx = /(^([\w ]+,?[\w ]+)?[\w ]+,?[\w ]+\d+:\d+(:\d+)?[\w ]?|^\d{1,4}[\/\-]\d{1,4}[\/\-]\d{1,4}|^\w+, \w+ \d+, \d{4})/;
const hexRx = /^0x[0-9a-f]+$/i;
const startsWithNullRx = /^\0/;
const endsWithNullRx = /\0$/;
const whitespaceRx = /\s+/g;
const startsWithZeroRx = /^0/;

loader.lazyGetter(this, "standardSessionString", () => {
  const l10n = new Localization(["devtools/client/storage.ftl"], true);
  return l10n.formatValueSync("storage-expires-session");
});

/**
 * Sort numbers, strings, IP Addresses, Dates, Filenames, version numbers etc.
 * "the way humans do."
 *
 * This function should only be called via naturalSortCaseSensitive or
 * naturalSortCaseInsensitive.
 *
 * e.g. [3, 2, 1, 10].sort(naturalSortCaseSensitive)
 *
 * @param  {Object} a
 *         Passed in by Array.sort(a, b)
 * @param  {Object} b
 *         Passed in by Array.sort(a, b)
 * @param  {Boolean} insensitive
 *         Should the search be case insensitive?
 */
// eslint-disable-next-line complexity
function naturalSort(a = "", b = "", insensitive = false) {
  let sessionString = standardSessionString;

  // Ensure we are working with trimmed strings
  a = (a + "").trim();
  b = (b + "").trim();

  if (insensitive) {
    a = a.toLowerCase();
    b = b.toLowerCase();
    sessionString = standardSessionString.toLowerCase();
  }

  // Chunk/tokenize - Here we split the strings into arrays or strings and
  // numbers.
  const aChunks = a
    .replace(tokenizeNumbersRx, "\0$1\0")
    .replace(startsWithNullRx, "")
    .replace(endsWithNullRx, "")
    .split("\0");
  const bChunks = b
    .replace(tokenizeNumbersRx, "\0$1\0")
    .replace(startsWithNullRx, "")
    .replace(endsWithNullRx, "")
    .split("\0");

  // Hex or date detection.
  const aHexOrDate =
    parseInt(a.match(hexRx), 16) || (aChunks.length !== 1 && Date.parse(a));
  const bHexOrDate =
    parseInt(b.match(hexRx), 16) ||
    (aHexOrDate && b.match(dateRx) && Date.parse(b)) ||
    null;

  if (
    (aHexOrDate || bHexOrDate) &&
    (a === sessionString || b === sessionString)
  ) {
    // We have a date and a session string. Move "Session" above the date
    // (for session cookies)
    if (a === sessionString) {
      return -1;
    } else if (b === sessionString) {
      return 1;
    }
  }

  // Try and sort Hex codes or Dates.
  if (bHexOrDate) {
    if (aHexOrDate < bHexOrDate) {
      return -1;
    } else if (aHexOrDate > bHexOrDate) {
      return 1;
    }
  }

  // Natural sorting through split numeric strings and default strings
  const aChunksLength = aChunks.length;
  const bChunksLength = bChunks.length;
  const maxLen = Math.max(aChunksLength, bChunksLength);

  for (let i = 0; i < maxLen; i++) {
    const aChunk = normalizeChunk(aChunks[i] || "", aChunksLength);
    const bChunk = normalizeChunk(bChunks[i] || "", bChunksLength);

    // Handle numeric vs string comparison - number < string
    if (isNaN(aChunk) !== isNaN(bChunk)) {
      return isNaN(aChunk) ? 1 : -1;
    }

    // If unicode use locale comparison
    // eslint-disable-next-line no-control-regex
    if (/[^\x00-\x80]/.test(aChunk + bChunk) && aChunk.localeCompare) {
      const comp = aChunk.localeCompare(bChunk);
      return comp / Math.abs(comp);
    }
    if (aChunk < bChunk) {
      return -1;
    } else if (aChunk > bChunk) {
      return 1;
    }
  }
  return null;
}

// Normalize spaces; find floats not starting with '0', string or 0 if not
// defined
const normalizeChunk = function(str, length) {
  return (
    ((!str.match(startsWithZeroRx) || length == 1) && parseFloat(str)) ||
    str.replace(whitespaceRx, " ").trim() ||
    0
  );
};

exports.naturalSortCaseSensitive = function naturalSortCaseSensitive(a, b) {
  return naturalSort(a, b, false);
};

exports.naturalSortCaseInsensitive = function naturalSortCaseInsensitive(a, b) {
  return naturalSort(a, b, true);
};
