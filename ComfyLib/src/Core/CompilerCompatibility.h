#pragma once

// Keep the old MSVC spelling at existing call sites while using bounded C APIs
// on other compilers. Array overloads infer the destination capacity.
#ifndef _MSC_VER
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <climits>
#include <strings.h>
#include <ctime>
using __time64_t = int64_t;
#define __forceinline inline
#define _Printf_format_string_
#define _byteswap_ushort __builtin_bswap16
#define _byteswap_ulong __builtin_bswap32
#define _byteswap_uint64 __builtin_bswap64
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define sscanf_s std::sscanf
inline int localtime_s(std::tm* result, const std::time_t* time)
{
    return localtime_r(time, result) ? 0 : errno;
}
inline int sprintf_s(char* buffer, size_t capacity, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int result = std::vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
    return result;
}
template<size_t Capacity, typename... Arguments>
int sprintf_s(char (&buffer)[Capacity], const char* format, Arguments... arguments)
{
    return std::snprintf(buffer, Capacity, format, arguments...);
}
inline int strcpy_s(char* buffer, size_t capacity, const char* source)
{
    const size_t length = std::strlen(source);
    if (length >= capacity)
    {
        if (capacity != 0)
            buffer[0] = '\0';
        return ERANGE;
    }
    std::memcpy(buffer, source, length + 1);
    return 0;
}
template<size_t Capacity>
int strcpy_s(char (&buffer)[Capacity], const char* source)
{
    return strcpy_s(buffer, Capacity, source);
}
inline int strcat_s(char* buffer, size_t capacity, const char* source)
{
    const size_t length = strnlen(buffer, capacity);
    if (length == capacity)
        return ERANGE;
    return strcpy_s(buffer + length, capacity - length, source);
}
template<size_t Capacity>
int strcat_s(char (&buffer)[Capacity], const char* source)
{
    return strcat_s(buffer, Capacity, source);
}
#endif
