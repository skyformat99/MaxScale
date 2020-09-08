/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-08-24
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once

#include <maxbase/ccdefs.hh>

#if defined (PCRE2_CODE_UNIT_WIDTH)
#error PCRE2_CODE_UNIT_WIDTH already defined. Do not define, and include <maxscale/pcre2.h>.
#else
#define PCRE2_CODE_UNIT_WIDTH 8
#endif

#include <pcre2.h>

#include <memory>
#include <string>

namespace maxbase
{

// Class for PCRE2 regular expressions
class Regex
{
public:

    /**
     * Constructs a regular expression
     *
     * The default values construct an empty regular expression that is valid but does not evaluate to true.
     * This is used to signify unconfigured regular expressions.
     *
     * @param pattern The pattern to use.
     * @param options PCRE2 options to use.
     */
    Regex(const std::string& pattern = "", uint32_t options = 0);

    /**
     * Constructs a regular expression from existing code
     *
     * @param pattern The pattern where the code was compiled from.
     * @param code    The compiled PCRE2 code.
     * @param options PCRE2 options.
     */
    Regex(const std::string& pattern, pcre2_code* code, uint32_t options = 0);

    Regex(const Regex& rhs) = default;
    Regex(Regex&& rhs) = default;
    Regex& operator=(const Regex& rhs) = default;
    Regex& operator=(Regex&& rhs) = default;

    /**
     * @return True if the pattern is empty i.e. the string `""`
     */
    bool empty() const;

    /**
     * @return True if the pattern is either empty or it is valid
     */
    explicit operator bool() const;

    /**
     * @return True if pattern was compiled successfully
     */
    bool valid() const;

    /**
     * @return The human-readable form of the pattern.
     */
    const std::string& pattern() const;

    /**
     * @return The error returned by PCRE2 for invalid patterns
     */
    const std::string& error() const;

    /**
     * Check if `str` matches this pattern
     *
     * @param str  String to match
     * @param data The match data to use. If null, uses built-in match data that is not lock-free.
     *
     * @return True if the string matches the pattern
     */
    bool match(const std::string& str) const;

    /**
     * Replace matches in `str` with given replacement this pattern
     *
     * @param str         String to match
     * @param replacement String to replace matches with
     * @param data        The match data to use. If null, uses built-in match data that is not lock-free.
     *
     * @return True if the string matches the pattern
     */
    std::string replace(const std::string& str, const std::string& replacement) const;

    /**
     * Set PCRE2 options
     *
     * @param options The options to set
     */
    void set_options(uint32_t options)
    {
        m_options = options;
    }

    /**
     * Get PCRE2 options
     *
     * @return Current PCRE2 options
     */
    uint32_t options() const
    {
        return m_options;
    }

    /**
     * Get compiled pattern
     *
     * The ownership of the pointer is not transferred and must not be freed.
     *
     * @return The compiled pattern if one has been successfully compiled, otherwise nullptr
     */
    pcre2_code* code() const
    {
        return m_code.get();
    }

private:
    std::string                 m_pattern;
    std::string                 m_error;
    uint32_t                    m_options = 0;
    std::shared_ptr<pcre2_code> m_code;
};

/**
 * Replace all occurrences of pattern in string
 *
 * @param re      Compiled pattern to use
 * @param subject Subject string
 * @param replace Replacement string
 * @param error   Pointer to std::string where any error messages are stored
 *
 * @return The replaced string or the original string if no replacement was made. Returns an empty string when
 * any PCRE2 error is encountered.
 */
std::string pcre2_substitute(pcre2_code* re, const std::string& subject,
                             const std::string& replace, std::string* error);
}
