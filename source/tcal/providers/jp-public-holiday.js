function vbaInt(value) {
  return Math.floor(value);
}

function getSpringEquinoxDay(year) {
  if (year > 1850 && year < 1900) {
    return vbaInt(20.8431 + 0.242194 * (year - 1980)) - vbaInt((year - 1980) / 4.0);
  }
  if (year > 1899 && year < 1980) {
    return vbaInt(20.8357 + 0.242194 * (year - 1980)) - vbaInt((year - 1980) / 4.0);
  }
  if (year > 1979 && year < 2100) {
    return vbaInt(20.8431 + 0.242194 * (year - 1980)) - vbaInt((year - 1980) / 4.0);
  }
  if (year > 2099 && year < 2151) {
    return vbaInt(21.851 + 0.242194 * (year - 1980)) - vbaInt((year - 1980) / 4.0);
  }
  return 20;
}

function getAutumnEquinoxDay(year) {
  if (year > 1850 && year < 1900) {
    return vbaInt(22.2588 + 0.242194 * (year - 1980)) - vbaInt((year - 1980) / 4.0);
  }
  if (year > 1899 && year < 1980) {
    return vbaInt(23.2588 + 0.242194 * (year - 1980)) - vbaInt((year - 1980) / 4.0);
  }
  if (year > 1979 && year < 2100) {
    return vbaInt(23.2488 + 0.242194 * (year - 1980)) - vbaInt((year - 1980) / 4.0);
  }
  if (year > 2099 && year < 2151) {
    return vbaInt(24.2488 + 0.242194 * (year - 1980)) - vbaInt((year - 1980) / 4.0);
  }
  return 23;
}

function hasOwn(obj, key) {
  return Object.prototype.hasOwnProperty.call(obj, key);
}

function pad2(value) {
  return value < 10 ? "0" + value : String(value);
}

function makeDateKey(year, month, day) {
  return String(year) + "-" + pad2(month) + "-" + pad2(day);
}

function parseDateKey(dateKey) {
  var parts = String(dateKey).split("-");
  return {
    year: parseInt(parts[0], 10),
    month: parseInt(parts[1], 10),
    day: parseInt(parts[2], 10)
  };
}

function addDaysToDateKey(dateKey, delta) {
  var parsed = parseDateKey(dateKey);
  var value = new Date(parsed.year, parsed.month - 1, parsed.day);
  value.setDate(value.getDate() + delta);
  return makeDateKey(value.getFullYear(), value.getMonth() + 1, value.getDate());
}

function isSundayDateKey(dateKey) {
  var parsed = parseDateKey(dateKey);
  return new Date(parsed.year, parsed.month - 1, parsed.day).getDay() === 0;
}

function getNthMondayDateKey(year, month, nth) {
  var firstDay = new Date(year, month - 1, 1);
  var offset = (1 - firstDay.getDay() + 7) % 7;
  var day = 1 + offset + 7 * (nth - 1);
  return makeDateKey(year, month, day);
}

function parseMonthDaySpec(spec) {
  var parts = String(spec).split("/");
  if (parts.length < 2) {
    throw new Error("Unsupported fixed spec: " + spec);
  }
  var dayMatch = /^(\d+)/.exec(parts[1]);
  if (!dayMatch) {
    throw new Error("Unsupported fixed spec: " + spec);
  }
  return {
    month: parseInt(parts[0], 10),
    day: parseInt(dayMatch[1], 10)
  };
}

function getRuleScope(rule) {
  return rule && rule.scope ? String(rule.scope) : "public";
}

function isObservedForPostRule(rule) {
  return getRuleScope(rule) !== "closure";
}

function buildBaseHolidayMap(year, manifest) {
  var rules = manifest && manifest.rules ? manifest.rules : [];
  var seenNames = {};
  var byDate = {};
  for (var i = 0; i < rules.length; ++i) {
    var rule = rules[i];
    var endYear = rule.endYear || year;
    if (year < rule.startYear || year > endYear) {
      continue;
    }
    if (hasOwn(seenNames, rule.name)) {
      continue;
    }
    var dateKey = "";
    if (rule.kind === "limited") {
      var parts = String(rule.dateSpec || "").split("/");
      if (parts.length !== 3) {
        continue;
      }
      if (parseInt(parts[0], 10) !== year) {
        continue;
      }
      dateKey = makeDateKey(year, parseInt(parts[1], 10), parseInt(parts[2], 10));
    } else if (rule.kind === "fixed") {
      var fixed = parseMonthDaySpec(rule.dateSpec || "");
      dateKey = makeDateKey(year, fixed.month, fixed.day);
    } else if (rule.kind === "monday_fixed") {
      dateKey = getNthMondayDateKey(year, rule.month, rule.nth);
    } else if (rule.kind === "spring_equinox") {
      dateKey = makeDateKey(year, 3, getSpringEquinoxDay(year));
    } else if (rule.kind === "autumn_equinox") {
      dateKey = makeDateKey(year, 9, getAutumnEquinoxDay(year));
    }
    if (dateKey) {
      byDate[dateKey] = {
        name: rule.name,
        observedForPostRule: isObservedForPostRule(rule)
      };
      seenNames[rule.name] = true;
    }
  }
  return byDate;
}

function getPostRuleStartYear(manifest, kind, defaultValue) {
  var postrules = manifest && manifest.postrules ? manifest.postrules : [];
  for (var i = 0; i < postrules.length; ++i) {
    if (postrules[i].kind === kind) {
      return postrules[i].startYear || defaultValue;
    }
  }
  return defaultValue;
}

function isCitizenHoliday(baseHolidays, manifest, dateKey) {
  var parsed = parseDateKey(dateKey);
  var startYear = getPostRuleStartYear(manifest, "citizen_holiday", 1948);
  if (parsed.year < startYear) {
    return false;
  }
  if (isSundayDateKey(dateKey)) {
    return false;
  }
  if (hasOwn(baseHolidays, dateKey)) {
    return false;
  }
  var prevKey = addDaysToDateKey(dateKey, -1);
  var nextKey = addDaysToDateKey(dateKey, 1);
  return hasOwn(baseHolidays, prevKey) &&
    hasOwn(baseHolidays, nextKey) &&
    baseHolidays[prevKey].observedForPostRule &&
    baseHolidays[nextKey].observedForPostRule;
}

function isHolidayWithoutSubstitute(baseHolidays, manifest, dateKey) {
  return hasOwn(baseHolidays, dateKey) || isCitizenHoliday(baseHolidays, manifest, dateKey);
}

function getSubstituteHolidayName(baseHolidays, manifest, dateKey) {
  var parsed = parseDateKey(dateKey);
  var startYear = getPostRuleStartYear(manifest, "substitute_holiday", 1973);
  if (parsed.year < startYear) {
    return "";
  }
  if (hasOwn(baseHolidays, dateKey)) {
    return "";
  }
  var scanKey = addDaysToDateKey(dateKey, -1);
  while (isHolidayWithoutSubstitute(baseHolidays, manifest, scanKey)) {
    var holiday = hasOwn(baseHolidays, scanKey) ? baseHolidays[scanKey] : null;
    if (holiday && holiday.observedForPostRule && holiday.name && isSundayDateKey(scanKey)) {
      return holiday.name + "\u632f\u66ff";
    }
    if (holiday && !holiday.observedForPostRule) {
      return "";
    }
    scanKey = addDaysToDateKey(scanKey, -1);
  }
  return "";
}

function getHolidays(year, manifest) {
  var rangeStart = manifest && manifest.rangeStart ? manifest.rangeStart : 0;
  var rangeEnd = manifest && manifest.rangeEnd ? manifest.rangeEnd : 0;
  if (rangeStart && year < rangeStart) {
    return [];
  }
  if (rangeEnd && year > rangeEnd) {
    return [];
  }
  var baseHolidays = buildBaseHolidayMap(year, manifest);
  var out = [];
  var dayValue = new Date(year, 0, 1);
  while (dayValue.getFullYear() === year) {
    var dateKey = makeDateKey(year, dayValue.getMonth() + 1, dayValue.getDate());
    if (hasOwn(baseHolidays, dateKey)) {
      out.push({ date: dateKey, name: baseHolidays[dateKey].name, kind: "public" });
    } else if (isCitizenHoliday(baseHolidays, manifest, dateKey)) {
      out.push({ date: dateKey, name: "国民の休日", kind: "citizen" });
    } else {
      var substituteName = getSubstituteHolidayName(baseHolidays, manifest, dateKey);
      if (substituteName) {
        out.push({ date: dateKey, name: substituteName, kind: "substitute" });
      }
    }
    dayValue.setDate(dayValue.getDate() + 1);
  }
  return out;
}
