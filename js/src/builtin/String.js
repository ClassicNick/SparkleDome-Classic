/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*global intl_Collator: false, */

/* ES6 Draft September 5, 2013 21.1.3.3 */
function String_codePointAt(pos) {
    // Steps 1-3.
    CheckObjectCoercible(this);
    var S = ToString(this);

    // Steps 4-5.
    var position = ToInteger(pos);

    // Step 6.
    var size = S.length;

    // Step 7.
    if (position < 0 || position >= size)
        return undefined;

    // Steps 8-9.
    var first = callFunction(std_String_charCodeAt, S, position);
    if (first < 0xD800 || first > 0xDBFF || position + 1 === size)
        return first;

    // Steps 10-11.
    var second = callFunction(std_String_charCodeAt, S, position + 1);
    if (second < 0xDC00 || second > 0xDFFF)
        return first;

    // Step 12.
    return (first - 0xD800) * 0x400 + (second - 0xDC00) + 0x10000;
}


var collatorCache = new Record();

/* ES6 20121122 draft 15.5.4.21. */
function String_repeat(count) {
    // Steps 1-3.
    CheckObjectCoercible(this);
    var S = ToString(this);

    // Steps 4-5.
    var n = ToInteger(count);

    // Steps 6-7.
    if (n < 0 || n === std_Number_POSITIVE_INFINITY)
        ThrowError(JSMSG_REPEAT_RANGE);

    // Step 8-9.
    if (n === 0)
        return "";

    var result = S;
    for (var i = 1; i <= n / 2; i *= 2)
        result += result;
    for (; i < n; i++)
        result += S;
    return result;
}

/**
 * Compare this String against that String, using the locale and collation
 * options provided.
 *
 * Spec: ECMAScript Internationalization API Specification, 13.1.1.
 */
function String_localeCompare(that) {
    // Steps 1-3.
    CheckObjectCoercible(this);
    var S = ToString(this);
    var That = ToString(that);

    // Steps 4-5.
    var locales = arguments.length > 1 ? arguments[1] : undefined;
    var options = arguments.length > 2 ? arguments[2] : undefined;

    // Step 6.
    var collator;
    if (locales === undefined && options === undefined) {
        // This cache only optimizes for the old ES5 localeCompare without
        // locales and options.
        if (collatorCache.collator === undefined)
            collatorCache.collator = intl_Collator(locales, options);
        collator = collatorCache.collator;
    } else {
        collator = intl_Collator(locales, options);
    }

    // Step 7.
    return intl_CompareStrings(collator, S, That);
}

/* ES6 Draft September 5, 2013 21.1.2.2 */
function String_static_fromCodePoint() {
    // Step 1. is not relevant
    // Step 2.
    var length = arguments.length;

    // Step 3.
    var elements = new List();

    // Step 4-5., 5g.
    for (var nextIndex = 0; nextIndex < length; nextIndex++) {
        // Step 5a.
        var next = arguments[nextIndex];
        // Step 5b-c.
        var nextCP = ToNumber(next);

        // Step 5d.
        if (nextCP !== ToInteger(nextCP) || std_isNaN(nextCP))
            ThrowError(JSMSG_NOT_A_CODEPOINT, ToString(nextCP));

        // Step 5e.
        if (nextCP < 0 || nextCP > 0x10FFFF)
            ThrowError(JSMSG_NOT_A_CODEPOINT, ToString(nextCP));

        // Step 5f.
        // Inlined UTF-16 Encoding
        if (nextCP <= 0xFFFF) {
            elements.push(nextCP);
            continue;
        }

        elements.push((((nextCP - 0x10000) / 0x400) | 0) + 0xD800);
        elements.push((nextCP - 0x10000) % 0x400 + 0xDC00);
    }

    // Step 6.
    return callFunction(std_Function_apply, std_String_fromCharCode, null, elements);
}

/**
 * Compare String str1 against String str2, using the locale and collation
 * options provided.
 *
 * Mozilla proprietary.
 * Spec: https://developer.mozilla.org/en-US/docs/JavaScript/Reference/Global_Objects/String#String_generic_methods
 */
function String_static_localeCompare(str1, str2) {
    if (arguments.length < 1)
        ThrowError(JSMSG_MISSING_FUN_ARG, 0, "String.localeCompare");
    var locales = arguments.length > 2 ? arguments[2] : undefined;
    var options = arguments.length > 3 ? arguments[3] : undefined;
    return callFunction(String_localeCompare, str1, str2, locales, options);
}
