#ifndef I18N_HPP
#define I18N_HPP

#include "i18n/base.hpp"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <libintl.h>
#include <tuple>
#include <type_traits>
#include <version>

#ifndef USE_FMT
  #if !defined(__cpp_lib_format) || __cpp_lib_format < 201907
    #define USE_FMT 1
  #endif
#endif

#if USE_FMT
  #include <fmt/format.h>
namespace fmtstd = fmt;
#else
  #include <format>
namespace fmtstd = std;
#endif

namespace mfk::i18n {

namespace detail {

// From the C++ standard:
#if __cpp_lib_concepts < 202002L
template<class From, class To>
concept convertible_to = std::is_convertible_v<From, To> && requires {
  static_cast<To>(std::declval<From>());
};
#else
using std::convertible_to;
#endif

template <typename Derived>
class I18NStringImpl {
 public:
  operator const char *() const {
    auto &self             = *static_cast<const Derived *>(this);
    auto &&msgid           = self.get_msgid();
    const char *translated = dgettext(self.get_domain(), msgid);
    return translated != msgid ? translated : self.get_singular();
  }
  operator std::string_view() const { return static_cast<const char *>(*this); }

  template <typename... Args>
  decltype(auto) operator()(Args &&...args) const {
    return fmtstd::vformat(std::string_view(*this),
                           fmtstd::make_format_args(std::forward<Args>(args)...));
  }

 protected:
  constexpr auto get_singular() const {
    const char *msgid = static_cast<const Derived *>(this)->get_msgid();
    auto singular     = strchr(msgid, '\4');
    return singular ? singular + 1 : msgid;
  }
};

template <typename Derived>
class I18NPluralStringImpl {
 public:
  const char *operator[](unsigned long n) const {
    auto &self             = *static_cast<const Derived *>(this);
    auto &&msgid           = self.get_msgid();
    const char *translated = dngettext(self.get_domain(), msgid, self.get_plural(), n);
    return translated != msgid ? translated : self.get_singular();
  }

  template <convertible_to<unsigned long> First, typename... Args>
  decltype(auto) operator()(First &&first, Args &&...args) const {
    return fmtstd::vformat((*this)[first], fmtstd::make_format_args(std::forward<First>(first),
                                                                    std::forward<Args>(args)...));
  }

 protected:
  constexpr auto get_singular() const {
    const char *msgid = static_cast<const Derived *>(this)->get_msgid();
    auto singular     = strchr(msgid, '\4');
    return singular ? singular + 1 : msgid;
  }
  constexpr auto get_plural() const {
    const char *msgid = static_cast<const Derived *>(this)->get_msgid();
    return msgid + std::strlen(msgid) + 1;
  }
};
} // namespace detail

class SmallI18NStringCrossDomain : public detail::I18NStringImpl<SmallI18NStringCrossDomain> {
  friend class I18NStringImpl;

 public:
  explicit constexpr SmallI18NStringCrossDomain(const char *domain, const char *msgid):
      domain(domain), msgid(msgid) {}

 protected:
  constexpr auto get_domain() const { return domain; }
  constexpr auto get_msgid() const { return msgid; }

  const char *domain;
  const char *msgid;
};

class I18NStringCrossDomain :
    public SmallI18NStringCrossDomain,
    public detail::I18NStringImpl<I18NStringCrossDomain> {
  using I18NStringImpl = detail::I18NStringImpl<I18NStringCrossDomain>;
  friend I18NStringImpl;

 public:
  explicit constexpr I18NStringCrossDomain(const char *domain, const char *msgid,
                                           const char *singular):
      SmallI18NStringCrossDomain(domain, msgid),
      singular(singular) {}
  using I18NStringImpl::operator const char *, I18NStringImpl::operator std::string_view,
      I18NStringImpl::operator();

 protected:
  constexpr auto get_singular() const { return singular; }
  const char *singular;
};

template <CompileTimeString Domain>
class SmallI18NString : public detail::I18NStringImpl<SmallI18NString<Domain>> {
  friend detail::I18NStringImpl<SmallI18NString>;

 public:
  explicit constexpr SmallI18NString(const char *msgid): msgid(msgid) {}
  constexpr operator SmallI18NStringCrossDomain() const {
    return SmallI18NStringCrossDomain(Domain.begin(), msgid);
  }

 protected:
  constexpr auto get_domain() const { return Domain.begin(); }
  constexpr auto get_msgid() const { return msgid; }

  const char *msgid;
};

template <CompileTimeString Domain>
class I18NString :
    public SmallI18NString<Domain>,
    public detail::I18NStringImpl<I18NString<Domain>> {
  using I18NStringImpl = detail::I18NStringImpl<I18NString>;
  friend I18NStringImpl;

 public:
  explicit constexpr I18NString(const char *msgid, const char *singular):
      SmallI18NString<Domain>(msgid), singular(singular) {}
  constexpr operator I18NStringCrossDomain() const {
    return I18NStringCrossDomain(Domain.begin(), this->msgid, singular);
  }
  using I18NStringImpl::operator const char *, I18NStringImpl::operator std::string_view,
      I18NStringImpl::operator();

 protected:
  constexpr auto get_singular() const { return singular; }
  const char *singular;
};

class SmallI18NPluralStringCrossDomain :
    public detail::I18NPluralStringImpl<SmallI18NPluralStringCrossDomain> {
  friend class I18NPluralStringImpl;

 public:
  explicit constexpr SmallI18NPluralStringCrossDomain(const char *domain, const char *msgid):
      domain(domain), msgid(msgid) {}

 protected:
  constexpr auto get_domain() const { return domain; }
  constexpr auto get_msgid() const { return msgid; }

  const char *domain;
  const char *msgid;
};

class I18NPluralStringCrossDomain :
    public SmallI18NPluralStringCrossDomain,
    public detail::I18NPluralStringImpl<I18NPluralStringCrossDomain> {
  using I18NPluralStringImpl = detail::I18NPluralStringImpl<I18NPluralStringCrossDomain>;
  friend I18NPluralStringImpl;

 public:
  explicit constexpr I18NPluralStringCrossDomain(const char *domain, const char *msgid,
                                                 const char *singular, const char *plural):
      SmallI18NPluralStringCrossDomain(domain, msgid),
      singular(singular), plural(plural) {}
  using I18NPluralStringImpl::operator[], I18NPluralStringImpl::operator();

 protected:
  constexpr auto get_singular() const { return singular; }
  constexpr auto get_plural() const { return plural; }
  const char *singular;
  const char *plural;
};

template <CompileTimeString Domain>
class SmallI18NPluralString : public detail::I18NPluralStringImpl<SmallI18NPluralString<Domain>> {
  friend detail::I18NPluralStringImpl<SmallI18NPluralString>;

 public:
  explicit constexpr SmallI18NPluralString(const char *msgid): msgid(msgid) {}
  constexpr operator SmallI18NPluralStringCrossDomain() const {
    return SmallI18NPluralStringCrossDomain(Domain.begin(), msgid);
  }

 protected:
  constexpr auto get_domain() const { return Domain.begin(); }
  constexpr auto get_msgid() const { return msgid; }

  const char *msgid;
};

template <CompileTimeString Domain>
class I18NPluralString :
    public SmallI18NPluralString<Domain>,
    public detail::I18NPluralStringImpl<I18NPluralString<Domain>> {
  using I18NPluralStringImpl = detail::I18NPluralStringImpl<I18NPluralString>;
  friend I18NPluralStringImpl;

 public:
  explicit constexpr I18NPluralString(const char *msgid, const char *singular, const char *plural):
      SmallI18NPluralString<Domain>(msgid), singular(singular), plural(plural) {}
  constexpr operator I18NPluralStringCrossDomain() const {
    return I18NPluralStringCrossDomain(Domain.begin(), this->msgid, singular, this->plural);
  }
  using I18NPluralStringImpl::operator[], I18NPluralStringImpl::operator();

 protected:
  constexpr auto get_singular() const { return singular; }
  constexpr auto get_plural() const { return plural; }
  const char *singular;
  const char *plural;
};

namespace detail {
// The context part uses quite simple escaping rules:
// Everything can be escaped by prepending a \ and the first
// unescaped | limits the context part.  All unescaped
// parens get discarded. Additionally a | counts as escaped
// if the last previous paren was a (. Especially this implies that | enclosed
// in parens are escaped if all parens are balanced.
template <typename Char>
constexpr std::tuple<const Char *, std::size_t> measure_context(const Char *const begin,
                                                                const Char *const end) {
  std::size_t count = 0;
  bool grouped      = false;
  for (auto iter = begin; end != iter; ++iter) {
    if (*iter == '\\' && iter + 1 != end)
      ++iter;
    else if (*iter == '(')
      grouped = true;
    else if (*iter == ')')
      grouped = false;
    else if (*iter == '|' && !grouped)
      return std::make_tuple(iter + 1, count);
    ++count;
  }
  return std::make_tuple(begin, -1);
}

// Here we take a shortcut: We don't have to track if a | is grouped, since we
// know the length we can just check if we need more characters
template <typename Char, std::size_t Length>
constexpr CompileTimeString<Char, Length> extract_context(const Char *iter) {
  if (Length == -1) return {};
  CompileTimeString<Char, Length> result;
  for (auto &c : result) {
    if (*iter == '\\') ++iter;
    c = *iter++;
  }
  *result.end() = '\0';
  return result;
}

// The main part uses more complicated escaping since we want to allow normal
// interpretation of parens as often as possible.
//
// PRECONDITION: !*end. Technically *end != 's' && *end != ')' is enough.
template <std::size_t Singular, std::size_t Plural, typename Char>
constexpr std::tuple<CompileTimeString<Char, Singular>, CompileTimeString<Char, Plural>>
extract_singular_plural(const Char *iter, const Char *const end) {
  std::tuple<CompileTimeString<Char, Singular>, CompileTimeString<Char, Plural>> result;
  Char *out_singular = std::get<0>(result).begin(), *out_plural = std::get<1>(result).begin();
  if (Plural == std::size_t(-1)) {
    while (end != iter) {
      if (*iter == '\\') ++iter;
      *out_singular++ = *iter++;
    }
    if (out_singular != std::get<0>(result).end()) throw "Assertion failed";
  }
  bool grouped = false;
  // The following two are only defined if grouped is true.
  Char *try_singular, *try_plural;
  for (; end != iter; ++iter) {
    if (grouped) {
      // If we find another '(', we treat this as a new group and assume that
      // the previous '(' should be written in both forms. Similar for a
      // potential '|'.
      if (try_plural && *iter == ')') {
        // The standard case. Just commit the try_... .
        grouped      = false;
        out_singular = try_singular;
        out_plural   = try_plural;
      } else if (!try_plural && *iter == '|') {
        try_plural = out_plural;
      } else if (*iter == '(' || *iter == '|' || *iter == ')') {
        // We found something unexpected. Revert the current try.
        std::copy_backward(out_singular, try_singular, try_singular + 1);
        *out_singular = '(';
        ++try_singular;
        if (try_plural) {
          *try_singular++ = '|';
          try_singular    = std::copy(out_plural, try_plural, try_singular);
          try_plural      = nullptr;
        }
        out_plural   = std::copy(out_singular, try_singular, out_plural);
        out_singular = try_singular;
        if (*iter != '(') {
          grouped         = false;
          *out_singular++ = *out_plural++ = *iter;
        }
      } else {
        if (*iter == '\\') ++iter;
        (try_plural ? *try_plural++ : *try_singular++) = *iter;
      }
    } else if (*iter == '(') {
      grouped      = true;
      try_singular = out_singular;
      if (iter[1] == 's' && iter[2] == ')')
        try_plural = out_plural;
      else
        try_plural = nullptr;
    } else {
      if (*iter == '\\') ++iter;
      *out_singular++ = *out_plural++ = *iter;
    }
  }
  return result;
}

// Now measure the corresponding lengths.
// Basically the same code, just reduced to counting
template <typename Char>
constexpr std::tuple<std::size_t, std::size_t> measure_singular_plural(const Char *iter,
                                                                       const Char *const end) {
  std::size_t singular = 0, plural = 0;
  bool grouped = false, has_plural = false;
  // The following two are only defined if grouped is true.
  std::size_t try_singular, try_plural;
  for (; end != iter; ++iter) {
    if (grouped) {
      // If we find another '(', we treat this as a new group and assume that
      // the previous '(' should be written in both forms. Similar for a
      // potential '|'.
      if (try_plural != static_cast<std::size_t>(-1) && *iter == ')') {
        // The standard case. Just commit the try_... .
        has_plural = true;
        grouped    = false;
        singular += try_singular;
        plural += try_plural;
      } else if (try_plural == static_cast<std::size_t>(-1) && *iter == '|') {
        try_plural = 0;
      } else if (*iter == '(' || *iter == '|' || *iter == ')') {
        // We found something unexpected. Revert the current try.
        ++try_singular;
        if (try_plural != static_cast<std::size_t>(-1)) {
          try_singular += 1 + try_plural;
          try_plural = -1;
        }
        plural += try_singular;
        singular += try_singular;
        if (*iter != '(') {
          grouped = false;
          ++singular, ++plural;
        }
      } else {
        if (*iter == '\\') ++iter;
        if (try_plural != static_cast<std::size_t>(-1))
          ++try_plural;
        else
          ++try_singular;
      }
    } else if (*iter == '(') {
      grouped      = true;
      try_singular = 0;
      if (iter[1] == 's' && iter[2] == ')')
        try_plural = 0;
      else
        try_plural = -1;
    } else
      ++singular, ++plural;
  }
  return std::make_tuple(singular, has_plural ? plural : std::size_t(-1));
}

template <CompileTimeString Domain, CompileTimeString Context, CompileTimeString Singular,
          CompileTimeString Plural>
struct MyI18NString :
    private CompileTimeI18NString<Domain, Context, Singular, Plural>,
    public std::conditional_t<!!Plural, I18NPluralString<Domain>, I18NString<Domain>> {
 private:
  using CTS = typename MyI18NString::CompileTimeI18NString;

 public:
  constexpr MyI18NString() requires(!!Plural):
      MyI18NString::I18NPluralString(CTS::msgid(), CTS::singular(), CTS::plural()) {}
  constexpr MyI18NString() requires(!Plural):
      MyI18NString::I18NString(CTS::msgid(), CTS::singular()) {}
  template <typename... Args>
  decltype(auto) operator()(Args &&...args) const {
    if (false) {
      (void)fmtstd::format(Singular.str, std::forward<Args>(args)...);
      if constexpr (Plural) (void)fmtstd::format(Plural.str, std::forward<Args>(args)...);
    }
    if constexpr (Plural)
      return MyI18NString::I18NPluralString::operator()(std::forward<Args>(args)...);
    else
      return MyI18NString::I18NString::operator()(std::forward<Args>(args)...);
  }
};
} // namespace detail

template <template <CompileTimeString, CompileTimeString, CompileTimeString, CompileTimeString>
          typename I18NStringBase,
          CompileTimeString Str,
          CompileTimeString Domain =
              CompileTimeString<typename decltype(Str)::char_type, std::size_t(-1)>()>
constexpr auto build_I18NString_generic() {
  using char_type = typename decltype(Str)::char_type;
  // We are working in two steps:
  constexpr auto lengths = [] {
    struct {
      std::size_t context = -1, singular = 0, plural = -1;
      const char_type *start_main;
    } result;
    std::tie(result.start_main, result.context) = detail::measure_context(Str.begin(), Str.end());
    std::tie(result.singular, result.plural) =
        detail::measure_singular_plural(result.start_main, Str.end());
    return result;
  }();

  struct TripleString {
    CompileTimeString<char_type, lengths.context> context;
    CompileTimeString<char_type, lengths.singular> singular;
    CompileTimeString<char_type, lengths.plural> plural;
  };
  constexpr TripleString strings = [&] {
    TripleString result{};
    result.context = detail::extract_context<char_type, lengths.context>(Str.begin());
    auto&& [singular, plural] =
        detail::extract_singular_plural<lengths.singular, lengths.plural>(lengths.start_main,
                                                                          Str.end());
    result.singular = singular;
    result.plural = plural;
    return result;
  }();

  return I18NStringBase<Domain, strings.context, strings.singular, strings.plural>();
}

template <CompileTimeString Str, CompileTimeString Domain = CompileTimeString<
                                     typename decltype(Str)::char_type, std::size_t(-1)>()>
constexpr auto build_I18NString() {
  return build_I18NString_generic<detail::MyI18NString, Str, Domain>();
}

} // namespace mfk::i18n

#endif
