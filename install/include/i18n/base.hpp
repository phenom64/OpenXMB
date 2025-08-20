#ifndef I18N_BASE_HPP
#define I18N_BASE_HPP

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>

#ifdef __has_attribute
#if __has_attribute(mfk_i18n)
#define I18N_ATTR(x) [[mfk::i18n ## x]]
#else
#define I18N_ATTR(x)
#endif
#else
#define I18N_ATTR(x)
#endif

namespace mfk::i18n {

// Char should be char, char8_t, char16_t or char32_t.
template <typename Char, std::size_t Length>
struct CompileTimeString {
  constexpr CompileTimeString() = default;
  constexpr CompileTimeString(const Char (&literal)[Length + 1]) {
    for (int i = 0; i != Length + 1; ++i)
      str[i] = literal[i];
    if (std::is_constant_evaluated()) {
      if (*end()) throw "Initializer for CompileTimeString should be NULL-terminated";
    } else {
      assert(!*end());
    }
  }
  constexpr Char *begin() { return str; }
  constexpr const Char *begin() const { return str; }
  constexpr Char *end() { return str + Length; }
  constexpr const Char *end() const { return str + Length; }
  constexpr explicit operator bool() const { return true; }

  using char_type                     = Char;
  Char str[Length + 1]                = {0};
  static constexpr std::size_t length = Length;
};

/* We use CompileTimeString<T, -1> to indicate non-existance */
template <typename Char>
struct CompileTimeString<Char, std::size_t(-1)> {
  constexpr CompileTimeString() = default;
  constexpr CompileTimeString(std::nullptr_t) {}

  constexpr Char *begin() { return nullptr; }
  constexpr const Char *begin() const { return nullptr; }
  constexpr Char *end() { return nullptr; }
  constexpr const Char *end() const { return nullptr; }
  constexpr explicit operator bool() const { return false; }

  using char_type                     = Char;
  static constexpr std::size_t length = -1;
  static constexpr char *str          = nullptr;
};

template <typename Char, std::size_t Length>
CompileTimeString(const Char (&)[Length]) -> CompileTimeString<Char, Length - 1>;

namespace detail {
// If one string does not exists, return the other (with no separator)
// Char1 == Char2, but using a single template argument would lead to issues during overload
// resolution
template <typename Char1, typename Char2, std::size_t Length1, std::size_t Length2>
requires(std::same_as<Char1, Char2> &&Length1 != -1 && Length2 != -1) constexpr CompileTimeString<
    Char1, Length1 + 1 + Length2> join_with_separator(CompileTimeString<Char1, Length1> str1,
                                                      Char1 sep,
                                                      CompileTimeString<Char2, Length2> str2) {
  CompileTimeString<Char1, Length1 + 1 + Length2> result;
  auto result_iter = std::copy(str1.begin(), str1.end(), result.begin());
  *result_iter++   = sep;
  result_iter      = std::copy(str2.begin(), str2.end(), result_iter);
  *result_iter     = '\0';
  /* assert(result_iter == result.end()); */
  return result;
}
template <typename Char1, typename Char2, std::size_t Length1>
constexpr CompileTimeString<Char1, Length1>
join_with_separator(CompileTimeString<Char1, Length1> str1, Char1,
                    CompileTimeString<Char2, std::size_t(-1)>) {
  return str1;
}
template <typename Char1, typename Char2, std::size_t Length2>
constexpr CompileTimeString<Char2, Length2>
join_with_separator(CompileTimeString<Char1, std::size_t(-1)>, Char2,
                    CompileTimeString<Char2, Length2> str2) {
  return str2;
}

} // namespace detail

template <CompileTimeString Domain, CompileTimeString Context, CompileTimeString Singular,
          CompileTimeString Plural>
class I18N_ATTR() CompileTimeI18NString {
 public:
  using char_type = typename decltype(Singular)::char_type;
  static_assert(Context.length == -1
                || std::is_same_v<typename decltype(Context)::char_type, char_type>);
  static_assert(Plural.length == -1
                || std::is_same_v<typename decltype(Plural)::char_type, char_type>);
  static_assert(Singular.length != -1, "Singular string required");

  static constexpr auto domain() { return domain_begin; }
  static constexpr auto msgid() { return idStorage.begin(); }
  static constexpr auto singular() { return singular_begin; }
  static constexpr auto plural() { return plural_begin; }

 private:
  static constexpr auto idStorage = detail::join_with_separator(
      detail::join_with_separator(Context, char_type('\4'), Singular), char_type('\0'), Plural);
  static constexpr auto domain_begin I18N_ATTR(_domain_begin) =
      Domain.length == -1 ? nullptr : Domain.begin();
  static constexpr auto domain_end I18N_ATTR(_domain_end) =
      domain_begin ? domain_begin + Domain.length : nullptr;
  static constexpr const char_type *context_begin I18N_ATTR(_context_begin) =
      Context.length == -1 ? nullptr : idStorage.begin();
  static constexpr const char_type *context_end I18N_ATTR(_context_end) =
      context_begin ? context_begin + Context.length : nullptr;
  static constexpr const char_type *singular_begin I18N_ATTR(_singular_begin) =
      idStorage.begin() + (Context.length + 1); // context_end+1 or idStorage.begin()
  static constexpr const char_type *singular_end I18N_ATTR(_singular_end) =
      singular_begin + Singular.length;
  static constexpr const char_type *plural_begin I18N_ATTR(_plural_begin) =
      Plural.length == -1 ? nullptr : singular_end + 1;
  static constexpr const char_type *plural_end I18N_ATTR(_plural_end) =
      plural_begin ? plural_begin + Plural.length : nullptr;
};

} // namespace mfk::i18n

#endif
