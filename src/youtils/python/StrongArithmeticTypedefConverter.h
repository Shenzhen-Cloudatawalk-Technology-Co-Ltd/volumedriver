#ifndef YPY_STRONG_ARITHMETIC_TYPEDEF_CONVERTER_H_
#define YPY_STRONG_ARITHMETIC_TYPEDEF_CONVERTER_H_

#include "Wrapper.h"

#include <typeinfo>
#include <type_traits>

#include <boost/filesystem/path.hpp>
#include <boost/python/object.hpp>
#include <boost/python/to_python_converter.hpp>

// pythonX.Y/patchlevel.h
#include <patchlevel.h>

#include <youtils/Logging.h>

namespace youtils
{

template<typename T>
struct StrongArithmeticTypedefConverter
{
    DECLARE_LOGGER("StrongArithmeticTypedefConverter");

    typedef decltype(T::t) arithmetic_type;

    static_assert(std::is_arithmetic<arithmetic_type>::value,
                  "only works with arithmetic types");
    static_assert(std::is_convertible<T, arithmetic_type>::value,
                  "what do you think this is?");

    // Sorry, only Python Ints for now. Other arithmetic types will be
    // added later on (potentially only once the need arises).
    static_assert(std::is_integral<arithmetic_type>::value,
                  "TODO: support for non-integral types");
    static_assert(sizeof(long) >= sizeof(arithmetic_type),
                  "TODO: support types other than Python Ints");

    static PyObject*
    convert(const T& t)
    {
        boost::python::object o(static_cast<arithmetic_type>(t));
        return boost::python::incref(o.ptr());
    }

    static void*
    convertible(PyObject* o)
    {
        if (
#if PY_MAJOR_VERSION < 3
            PyInt_Check(o)
#else
            PyLong_Check(o)
#endif
            )
        {
            long l =
#if PY_MAJOR_VERSION < 3
                       PyInt_AsLong(o)
#else
                       PyLong_AsLong(o)
#endif
                       ;

            if (l == -1)
            {
                if (PyErr_Occurred())
                {
                    PyErr_Print();
                }

                return nullptr;
            }

            if (l >= static_cast<long>(std::numeric_limits<arithmetic_type>::min()) and
                l <= static_cast<long>(std::numeric_limits<arithmetic_type>::max()))
            {
                return o;
            }
        }

        return nullptr;
    }

    static void
    from_python(PyObject* o,
                boost::python::converter::rvalue_from_python_stage1_data* data)
    {
        const long l =
#if PY_MAJOR_VERSION < 3
                       PyInt_AsLong(o)
#else
                       PyLong_AsLong(o)
#endif
            ;
        assert(not (l == -1 and PyErr_Occurred()));
        arithmetic_type t = l;

        typedef boost::python::converter::rvalue_from_python_storage<T> storage_type;

        void* storage = reinterpret_cast<storage_type*>(data)->storage.bytes;
        data->convertible = new(storage) T(t);
    }

    static void
    registerize()
    {
        boost::python::converter::registry::push_back(&StrongArithmeticTypedefConverter<T>::convertible,
                                                      &StrongArithmeticTypedefConverter<T>::from_python,
                                                      boost::python::type_id<T>());

        boost::python::to_python_converter<T, StrongArithmeticTypedefConverter<T>>();
    }

};

}

#define REGISTER_STRONG_ARITHMETIC_TYPEDEF_CONVERTER(T) \
    youtils::python::register_once<youtils::StrongArithmeticTypedefConverter<T>>()

#endif // !YPY_STRONG_ARITHMETIC_TYPEDEF_CONVERTER_H_
