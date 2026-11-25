#pragma once

// newlib on the Switch has no timegm, which ImPlot's date axes call (vendor/implot/implot.cpp);
// CMake includes this ahead of that file on the Switch only. UTC seconds for a broken-down time,
// by days from the civil date.
#include <ctime>

static inline time_t timegm(struct tm* t)
{
    long long year = static_cast<long long>(t->tm_year) + 1900;
    long long month = t->tm_mon + 1;
    year -= month <= 2 ? 1 : 0;
    long long era = (year >= 0 ? year : year - 399) / 400;
    long long yearOfEra = year - era * 400;
    long long dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + t->tm_mday - 1;
    long long dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    long long days = era * 146097 + dayOfEra - 719468;
    return static_cast<time_t>(days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
}
