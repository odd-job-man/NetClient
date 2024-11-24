#pragma once
#ifdef PARSERAPI
#else
#define PARSERAPI extern "C" __declspec(dllimport)
#endif 

#include <windows.h>
using PARSER = PVOID;

PARSERAPI PARSER CreateParser(LPCWSTR fileName);
PARSERAPI bool GetValue(PARSER parser, LPCWSTR key, PVOID* pStart, PVOID* pNextOfEnd);
PARSERAPI VOID ReleaseParser(PARSER parser);
