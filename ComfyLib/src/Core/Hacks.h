#pragma once
#include "Types.h"
#include <typeinfo>

namespace Comfy::Hacks
{
	// NOTE: This is without a doubt invoking undefined behavior but MSVC does not currently allow enabling RTTI for specified classes only
	//		 and from my testing with _MSV_VER=1916 this is reliable for both debug and release builds.

#ifdef _MSC_VER
    template <typename T>
    const void* GetVirtualFunctionTablePointer(const T& object)
    {
        static_assert(std::is_polymorphic_v<T>);
        return *reinterpret_cast<const void* const*>(&object);
    }
#endif

	template <typename BaseType>
	bool CompareVirtualFunctionTablePointers(const BaseType& polymorphicObjectA, const BaseType& polymorphicObjectB)
	{
		#ifdef _MSC_VER
        return GetVirtualFunctionTablePointer(polymorphicObjectA) == GetVirtualFunctionTablePointer(polymorphicObjectB);
#else
        return typeid(polymorphicObjectA) == typeid(polymorphicObjectB);
#endif
	}
}
