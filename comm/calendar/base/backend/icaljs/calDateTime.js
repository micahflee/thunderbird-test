/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* import-globals-from calICALJSComponents.js */

var { ICAL, unwrap, unwrapSetter } = ChromeUtils.import("resource:///modules/calendar/Ical.jsm");

var UNIX_TIME_TO_PRTIME = 1000000;

function calDateTime(innerObject) {
  this.wrappedJSObject = this;
  this.innerObject = innerObject || ICAL.Time.epochTime.clone();
}

calDateTime.prototype = {
  QueryInterface: ChromeUtils.generateQI(["calIDateTime"]),
  classID: Components.ID("{36783242-ec94-4d8a-9248-d2679edd55b9}"),

  isMutable: true,
  makeImmutable() {
    this.isMutable = false;
  },
  clone() {
    return new calDateTime(this.innerObject.clone());
  },

  isValid: true,
  innerObject: null,

  get nativeTime() {
    return this.innerObject.toUnixTime() * UNIX_TIME_TO_PRTIME;
  },
  set nativeTime(val) {
    this.innerObject.fromUnixTime(val / UNIX_TIME_TO_PRTIME);
  },

  get year() {
    return this.innerObject.year;
  },
  set year(val) {
    this.innerObject.year = val;
  },

  get month() {
    return this.innerObject.month - 1;
  },
  set month(val) {
    this.innerObject.month = val + 1;
  },

  get day() {
    return this.innerObject.day;
  },
  set day(val) {
    this.innerObject.day = val;
  },

  get hour() {
    return this.innerObject.hour;
  },
  set hour(val) {
    this.innerObject.hour = val;
  },

  get minute() {
    return this.innerObject.minute;
  },
  set minute(val) {
    this.innerObject.minute = val;
  },

  get second() {
    return this.innerObject.second;
  },
  set second(val) {
    this.innerObject.second = val;
  },

  get timezone() {
    return new calICALJSTimezone(this.innerObject.zone);
  },
  set timezone(rawval) {
    unwrapSetter(
      ICAL.Timezone,
      rawval,
      function(val) {
        this.innerObject.zone = val;
        return val;
      },
      this
    );
  },

  resetTo(year, month, day, hour, minute, second, timezone) {
    this.innerObject.fromData({
      year,
      month: month + 1,
      day,
      hour,
      minute,
      second,
    });
    this.timezone = timezone;
  },

  reset() {
    this.innerObject.reset();
  },

  get timezoneOffset() {
    return this.innerObject.utcOffset();
  },
  get isDate() {
    return this.innerObject.isDate;
  },
  set isDate(val) {
    this.innerObject.isDate = val;
  },

  get weekday() {
    return this.innerObject.dayOfWeek() - 1;
  },
  get yearday() {
    return this.innerObject.dayOfYear();
  },

  toString() {
    return this.innerObject.toString();
  },

  toJSON() {
    return this.toString();
  },

  getInTimezone: unwrap(ICAL.Timezone, function(val) {
    return new calDateTime(this.innerObject.convertToZone(val));
  }),

  addDuration: unwrap(ICAL.Duration, function(val) {
    this.innerObject.addDuration(val);
  }),

  subtractDate: unwrap(ICAL.Time, function(val) {
    return new calDuration(this.innerObject.subtractDateTz(val));
  }),

  compare: unwrap(ICAL.Time, function(val) {
    let a = this.innerObject;
    let b = val;

    // If either this or aOther is floating, both objects are treated
    // as floating for the comparison.
    if (a.zone == ICAL.Timezone.localTimezone || b.zone == ICAL.Timezone.localTimezone) {
      a = a.convertToZone(ICAL.Timezone.localTimezone);
      b = b.convertToZone(ICAL.Timezone.localTimezone);
    }

    if (a.isDate || b.isDate) {
      // Calendar expects 20120101 and 20120101T010101 to be equal
      return a.compareDateOnlyTz(b, a.zone);
    }
    // If both are dates or date-times, then just do the normal compare
    return a.compare(b);
  }),

  get startOfWeek() {
    return new calDateTime(this.innerObject.startOfWeek());
  },
  get endOfWeek() {
    return new calDateTime(this.innerObject.endOfWeek());
  },
  get startOfMonth() {
    return new calDateTime(this.innerObject.startOfMonth());
  },
  get endOfMonth() {
    return new calDateTime(this.innerObject.endOfMonth());
  },
  get startOfYear() {
    return new calDateTime(this.innerObject.startOfYear());
  },
  get endOfYear() {
    return new calDateTime(this.innerObject.endOfYear());
  },

  get icalString() {
    return this.innerObject.toICALString();
  },
  set icalString(val) {
    let jcalString;
    if (val.length > 10) {
      jcalString = ICAL.design.icalendar.value["date-time"].fromICAL(val);
    } else {
      jcalString = ICAL.design.icalendar.value.date.fromICAL(val);
    }
    this.innerObject = ICAL.Time.fromString(jcalString);
  },
};
