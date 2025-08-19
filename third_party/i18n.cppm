module;

#include <cstring>
#include <i18n/simple.hpp>

export module i18n;

export namespace mfk::i18n {
    inline namespace literals {
        using mfk::i18n::literals::operator""_;
    }
}
