#pragma once

#define CHAR_BIT __CHAR_BIT__

#define SCHAR_MIN (-__SCHAR_MAX__ - 1)
#define SCHAR_MAX __SCHAR_MAX__
#define UCHAR_MAX __UCHAR_MAX__

#if defined(__CHAR_UNSIGNED__)
#define CHAR_MIN 0
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#endif

#define SHRT_MIN (-__SHRT_MAX__ - 1)
#define SHRT_MAX __SHRT_MAX__
#define USHRT_MAX __USHRT_MAX__

#define INT_MIN (-__INT_MAX__ - 1)
#define INT_MAX __INT_MAX__
#define UINT_MAX __UINT_MAX__

#define LONG_MIN (-__LONG_MAX__ - 1)
#define LONG_MAX __LONG_MAX__
#define ULONG_MAX __ULONG_MAX__

#define LLONG_MIN (-__LONG_LONG_MAX__ - 1)
#define LLONG_MAX __LONG_LONG_MAX__
#define ULLONG_MAX __ULONG_LONG_MAX__
