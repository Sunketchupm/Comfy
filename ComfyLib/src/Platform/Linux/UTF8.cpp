#include "Misc/UTF8.h"
#include <codecvt>
#include <locale>

namespace Comfy::UTF8
{
    std::string Narrow(std::wstring_view input)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.to_bytes(input.data(), input.data() + input.size());
    }
    std::wstring Widen(std::string_view input)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.from_bytes(input.data(), input.data() + input.size());
    }
    WideArg::WideArg(std::string_view input)
    {
        const auto wide = Widen(input);
        convertedLength = wide.size();
        wchar_t* destination = stackBuffer.data();
        if (wide.size() >= stackBuffer.size())
        {
            heapBuffer = std::make_unique<wchar_t[]>(wide.size() + 1);
            destination = heapBuffer.get();
        }
        std::copy(wide.begin(), wide.end(), destination);
        destination[wide.size()] = L'\0';
    }
    const wchar_t* WideArg::c_str() const
    {
        return heapBuffer ? heapBuffer.get() : stackBuffer.data();
    }
}
