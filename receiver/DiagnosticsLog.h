#pragma once
#include <cstdarg>
#include <cstdio>

void DiagnosticsLog_Write(FILE* file, const char* fmt, va_list args);
void DiagnosticsLog_Append(const char* fileName, const char* fmt, ...);
