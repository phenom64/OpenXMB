#ifndef I18N_SIMPLE_HPP
#define I18N_SIMPLE_HPP

#include "../i18n.hpp"

extern "C" {
I18N_ATTR() extern char *gettext(I18N_ATTR(_singular_begin) const char *);
I18N_ATTR() extern char *dgettext(I18N_ATTR(_domain_begin) const char *,
                                    I18N_ATTR(_singular_begin) const char *);
I18N_ATTR() extern char *ngettext(I18N_ATTR(_singular_begin) const char *,
                                    I18N_ATTR(_plural_begin) const char *, unsigned long);
I18N_ATTR() extern char *dngettext(I18N_ATTR(_domain_begin) const char *,
                                     I18N_ATTR(_singular_begin) const char *,
                                     I18N_ATTR(_plural_begin) const char *, unsigned long);
}

namespace mfk::i18n::inline literals {
template <CompileTimeString str>
I18N_ATTR() constexpr auto operator""_() {
  return build_I18NString<str>();
}
} // namespace mfk::i18n::inline literals

#endif
