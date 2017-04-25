/*
  execution.cpp

  This file is part of GammaRay, the Qt application inspection and
  manipulation tool.

  Copyright (C) 2017 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Volker Krause <volker.krause@kdab.com>

  Licensees holding valid commercial KDAB GammaRay licenses may use this file in
  accordance with GammaRay Commercial License Agreement provided with the Software.

  Contact info@kdab.com if any conditions of this licensing are not clear to you.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config-gammaray.h>
#include "execution.h"

#include <QtGlobal>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <backward.hpp>
#define USE_BACKWARD_CPP
#else
#undef USE_BACKWARD_CPP
#endif

#if defined(Q_CC_MSVC)
#define USE_STACKWALKER
#include <StackWalker/StackWalker.h>
#else
#undef USE_STACKWALKER
#endif

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#endif
#ifdef HAVE_CXA_DEMANGLE
#include <cxxabi.h>
#endif

using namespace GammaRay;

namespace GammaRay {
namespace Execution {

#ifdef USE_BACKWARD_CPP
typedef backward::StackTrace TraceData;
#elif defined(Q_OS_WIN)
typedef QVector<ResolvedFrame> TraceData;
#else
typedef QVector<void*> TraceData;
#endif

class TracePrivate {
public:
    static TraceData& get(const Trace &trace)
    {
        return trace.d->data;
    }

    TraceData data;
};

}}

#ifndef Q_OS_WIN
//BEGIN UNIX specific code

#include <dlfcn.h>

bool Execution::isReadOnlyData(const void* data)
{
    Dl_info info;
    // ### technically we would also need to check if we are in a read-only section, but this close enough for our purpose
    return dladdr(const_cast<void*>(data), &info) != 0;
}

bool Execution::stackTracingAvailable()
{
#if defined(USE_BACKWARD_CPP) || defined(HAVE_BACKTRACE)
    return true;
#else
    return false;
#endif
}

bool Execution::hasFastStackTrace()
{
    return true;
}

Execution::Trace Execution::stackTrace(int maxDepth)
{
    Trace t;
#ifdef USE_BACKWARD_CPP
    TracePrivate::get(t).load_here(maxDepth);
#elif defined(HAVE_BACKTRACE)
    auto &v = TracePrivate::get(t);
    v.resize(maxDepth);
    const auto size = backtrace(v.data(), maxDepth);
    if (size <= 0)
        v.clear();
    else
        v.resize(size);
#else
    Q_UNUSED(maxDepth);
#endif
    return t;
}

#ifdef USE_BACKWARD_CPP
static backward::TraceResolver* resolver()
{
    static backward::TraceResolver s_traceResolver;
    return &s_traceResolver;
}

static Execution::ResolvedFrame toResolvedFrame(const backward::ResolvedTrace &resolvedTrace, void *addr)
{
    Execution::ResolvedFrame frame;
    // there's also resolvedTrace.location.name, but that doesn't give us fully qualified C++ names
    if (!resolvedTrace.object_function.empty())
        frame.name = QString::fromStdString(resolvedTrace.object_function);
    else if (!resolvedTrace.object_filename.empty())
        frame.name = QString::fromStdString(resolvedTrace.object_filename);
    else
        frame.name = QString::number(reinterpret_cast<quintptr>(addr), 16);
    frame.location.setUrl(QUrl::fromLocalFile(QString::fromStdString(resolvedTrace.source.filename)));
    frame.location.setOneBasedLine(resolvedTrace.source.line);
    frame.location.setOneBasedColumn(resolvedTrace.source.col);
    return frame;
}
#endif

#if !defined(USE_BACKWARD_CPP) && defined(HAVE_BACKTRACE)
static QString maybeDemangleName(char *name)
{
#ifdef HAVE_CXA_DEMANGLE
    auto mangledNameStart = strstr(name, "(_Z");
    if (mangledNameStart) {
        ++mangledNameStart;
        const auto mangledNameEnd = strstr(mangledNameStart, "+");
        if (mangledNameEnd) {
            int status;
            *mangledNameEnd = 0;
            auto demangled = abi::__cxa_demangle(mangledNameStart, nullptr, nullptr, &status);
            *mangledNameEnd = '+';
            if (status == 0 && demangled) {
                QString ret = QString::fromLatin1(name, mangledNameStart - name)
                              +QString::fromLatin1(demangled)
                              +QString::fromLatin1(mangledNameEnd);
                free(demangled);
                return ret;
            }
        }
    }
#endif
    return QString::fromLatin1(name);
}
#endif

Execution::ResolvedFrame Execution::resolveOne(const Execution::Trace &trace, int index)
{
    ResolvedFrame frame;

#ifdef USE_BACKWARD_CPP
    auto &st = TracePrivate::get(trace);
    resolver()->load_stacktrace(st);
    frame = toResolvedFrame(resolver()->resolve(st[index]), st[index].addr);

#elif defined(HAVE_BACKTRACE)
    const auto &v = TracePrivate::get(trace);
    char **strings = backtrace_symbols(v.data() + index, 1);
    frame.name = maybeDemangleName(strings[0]);
    free(strings);

#else
    Q_UNUSED(trace);
    Q_UNUSED(index);
#endif
    return frame;
}

QVector<Execution::ResolvedFrame> Execution::resolveAll(const Execution::Trace &trace)
{
    QVector<ResolvedFrame> frames;
    frames.reserve(trace.size());

#ifdef USE_BACKWARD_CPP
    auto &st = TracePrivate::get(trace);
    resolver()->load_stacktrace(st);
    for (int i = 0; i < trace.size(); ++i)
        frames.push_back(toResolvedFrame(resolver()->resolve(st[i]), st[i].addr));

#elif defined(HAVE_BACKTRACE)
    const auto &v = TracePrivate::get(trace);
    char **strings = backtrace_symbols(v.data(), v.size());
    for (int i = 0; i < v.size(); ++i) {
        ResolvedFrame frame;
        frame.name = maybeDemangleName(strings[i]);
        frames.push_back(frame);
    }
    free(strings);

#endif
    return frames;
}

//END Unix specific code
#else
//BEGIN Windows specific code

#include <qt_windows.h>

bool Execution::isReadOnlyData(const void* data)
{
    HMODULE handle;
    // ### technically we would also need to check if we are in a read-only section, but this close enough for our purpose
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(data), &handle);
}

bool Execution::stackTracingAvailable()
{
#ifdef USE_STACKWALKER
    return true;
#else
    return false;
#endif
}

bool Execution::hasFastStackTrace()
{
    return false;
}

#ifdef USE_STACKWALKER
class ResolvingStackWalker : public StackWalker
{
public:
    void stackTrace(QVector<Execution::ResolvedFrame> *frames)
    {
        m_frames = frames;
        ShowCallstack();
    }

protected:
    void OnSymInit(LPCSTR, DWORD, LPCSTR) override {}
    void OnLoadModule(LPCSTR, LPCSTR, DWORD64, DWORD, DWORD, LPCSTR, LPCSTR, ULONGLONG) override {}

    void OnCallstackEntry(CallstackEntryType eType, CallstackEntry &entry) override
    {
        if (eType == lastEntry || entry.offset == 0)
            return;

        Execution::ResolvedFrame frame;
        frame.name = entry.name;
        if (frame.name.isEmpty())
            frame.name = entry.moduleName;
        if (frame.name.isEmpty())
            frame.name = QString::number(entry.offset, 16);
        frame.location.setUrl(QUrl::fromLocalFile(entry.lineFileName));
        frame.location.setOneBasedLine(entry.lineNumber);
        m_frames->push_back(frame);
    }

private:
    QVector<Execution::ResolvedFrame> *m_frames;
};

#endif

Execution::Trace Execution::stackTrace(int maxDepth)
{
    Trace t;
#ifdef USE_STACKWALKER
    static ResolvingStackWalker s_stackWalker;

    auto &v = TracePrivate::get(t);
    s_stackWalker.stackTrace(&v);
#else
    Q_UNUSED(maxDepth);
#endif
    return t;
}

Execution::ResolvedFrame Execution::resolveOne(const Execution::Trace &trace, int index)
{
    return TracePrivate::get(trace).at(index);
}

QVector<Execution::ResolvedFrame> Execution::resolveAll(const Execution::Trace &trace)
{
    QVector<ResolvedFrame> frames;
    frames.reserve(trace.size());
    for (int i = 0; i < trace.size(); ++i)
        frames.push_back(resolveOne(trace, i));
    return frames;
}

//END Windows specific Code
#endif

//BEGIN generic code applicable for all platforms
namespace GammaRay {
namespace Execution {
Trace::Trace()
    : d(new TracePrivate)
{
}

Trace::Trace(const Trace &other)
    : d(other.d)
{
}

Trace::~Trace()
{
}

Trace& Trace::operator=(const Trace &other)
{
    d = other.d;
    return *this;
}

int Trace::size() const
{
    return d->data.size();
}

}}
//END generic code
