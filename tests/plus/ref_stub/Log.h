// SPDX-License-Identifier: GPL-3.0-or-later
// Host-test stand-in for the reference MMDVMHost logger (tests/plus/test_ysf_ref.cpp): every log call does nothing.
#pragma once
#define LogDebug(...) ((void)0)
#define LogMessage(...) ((void)0)
#define LogInfo(...) ((void)0)
#define LogWarning(...) ((void)0)
#define LogError(...) ((void)0)
#define LogFatal(...) ((void)0)
