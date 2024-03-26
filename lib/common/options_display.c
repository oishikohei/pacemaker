/*
 * Copyright 2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <glib.h>   // GSList, GString

#include "crmcommon_private.h"

/*!
 * \internal
 * \brief Output an option's possible values
 *
 * \param[in,out] out     Output object
 * \param[in]     option  Option whose possible values to add
 */
static void
add_possible_values_default(pcmk__output_t *out,
                            const pcmk__cluster_option_t *option)
{
    const char *id = _("Possible values");
    GString *buf = g_string_sized_new(256);

    CRM_ASSERT(option->type != NULL);

    if (pcmk_is_set(option->flags, pcmk__opt_generated)) {
        id = _("Possible values (generated by Pacemaker)");
    }

    if ((option->values != NULL) && (strcmp(option->type, "select") == 0)) {
        const char *delim = ", ";
        bool found_default = (option->default_value == NULL);
        char *str = pcmk__str_copy(option->values);

        for (const char *value = strtok(str, delim); value != NULL;
             value = strtok(NULL, delim)) {

            if (buf->len > 0) {
                g_string_append(buf, delim);
            }
            g_string_append_c(buf, '"');
            g_string_append(buf, value);
            g_string_append_c(buf, '"');

            if (!found_default && (strcmp(value, option->default_value) == 0)) {
                found_default = true;
                g_string_append(buf, _(" (default)"));
            }
        }
        free(str);

    } else if (option->default_value != NULL) {
        pcmk__g_strcat(buf,
                       option->type, _(" (default: \""), option->default_value,
                       "\")", NULL);

    } else {
        pcmk__g_strcat(buf, option->type, _(" (no default)"), NULL);
    }

    out->list_item(out, id, "%s", buf->str);
    g_string_free(buf, TRUE);
}

/*!
 * \internal
 * \brief Output a single option's metadata
 *
 * \param[in,out] out     Output object
 * \param[in]     option  Option to add
 */
static void
add_option_metadata_default(pcmk__output_t *out,
                            const pcmk__cluster_option_t *option)
{
    const char *desc_short = option->description_short;
    const char *desc_long = option->description_long;

    CRM_ASSERT((desc_short != NULL) || (desc_long != NULL));

    if (desc_short == NULL) {
        desc_short = desc_long;
        desc_long = NULL;
    }

    out->list_item(out, option->name, "%s", _(desc_short));

    out->begin_list(out, NULL, NULL, NULL);

    if (desc_long != NULL) {
        out->list_item(out, NULL, "%s", _(desc_long));
    }
    add_possible_values_default(out, option);
    out->end_list(out);
}

/*!
 * \internal
 * \brief Output the metadata for a list of options
 *
 * \param[in,out] out   Output object
 * \param[in]     args  Message-specific arguments
 *
 * \return Standard Pacemaker return code
 *
 * \note \p args should contain the following:
 *       -# Fake resource agent name for the option list (ignored)
 *       -# Short description of option list
 *       -# Long description of option list
 *       -# Filter: Group of <tt>enum pcmk__opt_flags</tt>; output an option
 *          only if its \c flags member has all these flags set
 *       -# <tt>NULL</tt>-terminated list of options whose metadata to format
 *       -# All: If \c true, output all options; otherwise, exclude advanced and
 *          deprecated options unless \c pcmk__opt_advanced and
 *          \c pcmk__opt_deprecated flags (respectively) are set in the filter.
 */
PCMK__OUTPUT_ARGS("option-list", "const char *", "const char *", "const char *",
                  "uint32_t", "const pcmk__cluster_option_t *", "bool")
static int
option_list_default(pcmk__output_t *out, va_list args)
{
    const char *name G_GNUC_UNUSED = va_arg(args, const char *);
    const char *desc_short = va_arg(args, const char *);
    const char *desc_long = va_arg(args, const char *);
    const uint32_t filter = va_arg(args, uint32_t);
    const pcmk__cluster_option_t *option_list =
        va_arg(args, pcmk__cluster_option_t *);
    const bool all = (bool) va_arg(args, int);

    const bool show_deprecated = all
                                 || pcmk_is_set(filter, pcmk__opt_deprecated);
    const bool show_advanced = all || pcmk_is_set(filter, pcmk__opt_advanced);
    bool old_fancy = false;

    GSList *deprecated = NULL;
    GSList *advanced = NULL;

    CRM_ASSERT((out != NULL) && (desc_short != NULL) && (desc_long != NULL)
               && (option_list != NULL));

    old_fancy = pcmk__output_text_get_fancy(out);
    pcmk__output_text_set_fancy(out, true);

    out->info(out, "%s", _(desc_short));
    out->spacer(out);
    out->info(out, "%s", _(desc_long));
    out->begin_list(out, NULL, NULL, NULL);

    for (const pcmk__cluster_option_t *option = option_list;
         option->name != NULL; option++) {

        // Store deprecated and advanced options to display later if appropriate
        if (pcmk_all_flags_set(option->flags, filter)) {
            if (pcmk_is_set(option->flags, pcmk__opt_deprecated)) {
                if (show_deprecated) {
                    deprecated = g_slist_prepend(deprecated, (gpointer) option);
                }

            } else if (pcmk_is_set(option->flags, pcmk__opt_advanced)) {
                if (show_advanced) {
                    advanced = g_slist_prepend(advanced, (gpointer) option);
                }

            } else {
                out->spacer(out);
                add_option_metadata_default(out, option);
            }
        }
    }

    if (advanced != NULL) {
        advanced = g_slist_reverse(advanced);

        out->spacer(out);
        out->begin_list(out, NULL, NULL, _("ADVANCED OPTIONS"));
        for (const GSList *iter = advanced; iter != NULL; iter = iter->next) {
            const pcmk__cluster_option_t *option = iter->data;

            out->spacer(out);
            add_option_metadata_default(out, option);
        }
        out->end_list(out);
        g_slist_free(advanced);
    }

    if (deprecated != NULL) {
        deprecated = g_slist_reverse(deprecated);

        out->spacer(out);
        out->begin_list(out, NULL, NULL,
                        _("DEPRECATED OPTIONS (will be removed in a future "
                          "release)"));
        for (const GSList *iter = deprecated; iter != NULL; iter = iter->next) {
            const pcmk__cluster_option_t *option = iter->data;

            out->spacer(out);
            add_option_metadata_default(out, option);
        }
        out->end_list(out);
        g_slist_free(deprecated);
    }

    out->end_list(out);
    pcmk__output_text_set_fancy(out, old_fancy);
    return pcmk_rc_ok;
}

/*!
 * \internal
 * \brief Add a description element to an OCF-like metadata XML node
 *
 * Include a translation based on the current locale if \c ENABLE_NLS is
 * defined.
 *
 * \param[in,out] out       Output object
 * \param[in]     for_long  If \c true, add long description; otherwise, add
 *                          short description
 * \param[in]     desc      Textual description to add
 */
static void
add_desc_xml(pcmk__output_t *out, bool for_long, const char *desc)
{
    const char *tag = (for_long? PCMK_XE_LONGDESC : PCMK_XE_SHORTDESC);
    xmlNode *node = pcmk__output_create_xml_text_node(out, tag, desc);

    crm_xml_add(node, PCMK_XA_LANG, PCMK__VALUE_EN);

#ifdef ENABLE_NLS
    {
        static const char *locale = NULL;

        if (strcmp(desc, _(desc)) == 0) {
            return;
        }

        if (locale == NULL) {
            locale = strtok(setlocale(LC_ALL, NULL), "_");
        }
        node = pcmk__output_create_xml_text_node(out, tag, _(desc));
        crm_xml_add(node, PCMK_XA_LANG, locale);
    }
#endif
}

/*!
 * \internal
 * \brief Output an option's possible values
 *
 * Add a \c PCMK_XE_OPTION element for each of the option's possible values.
 *
 * \param[in,out] out     Output object
 * \param[in]     option  Option whose possible values to add
 */
static void
add_possible_values_xml(pcmk__output_t *out,
                        const pcmk__cluster_option_t *option)
{
    if ((option->values != NULL) && (strcmp(option->type, "select") == 0)) {
        const char *delim = ", ";
        char *str = pcmk__str_copy(option->values);
        const char *ptr = strtok(str, delim);

        while (ptr != NULL) {
            pcmk__output_create_xml_node(out, PCMK_XE_OPTION,
                                         PCMK_XA_VALUE, ptr,
                                         NULL);
            ptr = strtok(NULL, delim);
        }
        free(str);
    }
}

/*!
 * \internal
 * \brief Map an option type to one suitable for daemon metadata
 *
 * \param[in] type  Option type to map
 *
 * \return String suitable for daemon metadata to display as an option type
 */
static const char *
map_legacy_option_type(const char *type)
{
    // @COMPAT Drop this function when we drop daemon metadata commands
    if (pcmk__str_eq(type, PCMK_VALUE_DURATION, pcmk__str_none)) {
        return PCMK__VALUE_TIME;

    } else if (pcmk__str_eq(type, PCMK_VALUE_NONNEGATIVE_INTEGER,
                            pcmk__str_none)) {
        return PCMK_VALUE_INTEGER;

    } else {
        return type;
    }
}

/*!
 * \internal
 * \brief Add a \c PCMK_XE_PARAMETER element to an OCF-like metadata XML node
 *
 * \param[in,out] out     Output object
 * \param[in]     option  Option to add as a \c PCMK_XE_PARAMETER element
 */
static void
add_option_metadata_xml(pcmk__output_t *out,
                        const pcmk__cluster_option_t *option)
{
    const char *type = option->type;
    const char *desc_long = option->description_long;
    const char *desc_short = option->description_short;
    const bool advanced = pcmk_is_set(option->flags, pcmk__opt_advanced);
    const bool deprecated = pcmk_is_set(option->flags, pcmk__opt_deprecated);
    const bool generated = pcmk_is_set(option->flags, pcmk__opt_generated);

    // OCF requires "1"/"0" and does not allow "true"/"false
    // @COMPAT Variables no longer needed after we drop legacy mode
    const char *advanced_s = advanced? "1" : "0";
    const char *generated_s = generated? "1" : "0";

    // @COMPAT For daemon metadata only; drop when daemon metadata is dropped
    const bool legacy = pcmk__output_get_legacy_xml(out);
    char *desc_long_legacy = NULL;
    GString *desc_short_legacy = NULL;

    // The standard requires a parameter type
    CRM_ASSERT(type != NULL);

    // The standard requires long and short parameter descriptions
    CRM_ASSERT((desc_long != NULL) || (desc_short != NULL));

    if (desc_long == NULL) {
        desc_long = desc_short;
    } else if (desc_short == NULL) {
        desc_short = desc_long;
    }

    if (legacy) {
        // This is ugly but it will go away at a major release bump
        type = map_legacy_option_type(type);

        if (option->values != NULL) {
            desc_long_legacy = crm_strdup_printf("%s  Allowed values: %s",
                                                 desc_long, option->values);
            desc_long = desc_long_legacy;
        }

        if (deprecated || advanced) {
            const size_t init_sz = 1023;

            if (desc_long != option->description_long) {
                /* desc_long was NULL and got assigned desc_short, which was
                 * non-empty. Let desc_long have the "real" description, and put
                 * the flag in desc_short.
                 */
                desc_short = "";
            } else {
                desc_short = pcmk__s(option->description_short, "");
            }

            if (deprecated) {
                pcmk__add_separated_word(&desc_short_legacy, init_sz,
                                         "*** Deprecated ***", NULL);
            }
            if (advanced) {
                pcmk__add_separated_word(&desc_short_legacy, init_sz,
                                         "*** Advanced Use Only ***", NULL);
            }
            pcmk__add_separated_word(&desc_short_legacy, 0, desc_short, NULL);

            desc_short = desc_short_legacy->str;
        }

        /* These must be NULL when used as attribute values later.
         * PCMK_XA_ADVANCED and PCMK_XA_GENERATED break validation for some
         * legacy tools.
         */
        advanced_s = NULL;
        generated_s = NULL;
    }

    pcmk__output_xml_create_parent(out, PCMK_XE_PARAMETER,
                                   PCMK_XA_NAME, option->name,
                                   PCMK_XA_ADVANCED, advanced_s,
                                   PCMK_XA_GENERATED, generated_s,
                                   NULL);

    if (deprecated && !legacy) {
        // No need yet to support "replaced-with" or "desc"; add if needed
        pcmk__output_create_xml_node(out, PCMK_XE_DEPRECATED, NULL);
    }
    add_desc_xml(out, true, desc_long);
    add_desc_xml(out, false, desc_short);

    pcmk__output_xml_create_parent(out, PCMK_XE_CONTENT,
                                   PCMK_XA_TYPE, type,
                                   PCMK_XA_DEFAULT, option->default_value,
                                   NULL);

    add_possible_values_xml(out, option);

    pcmk__output_xml_pop_parent(out);
    pcmk__output_xml_pop_parent(out);

    free(desc_long_legacy);
    if (desc_short_legacy != NULL) {
        g_string_free(desc_short_legacy, TRUE);
    }
}

/*!
 * \internal
 * \brief Output the metadata for a list of options as OCF-like XML
 *
 * \param[in,out] out   Output object
 * \param[in]     args  Message-specific arguments
 *
 * \return Standard Pacemaker return code
 *
 * \note \p args should contain the following:
 *       -# Fake resource agent name for the option list
 *       -# Short description of option list
 *       -# Long description of option list
 *       -# Filter: Group of <tt>enum pcmk__opt_flags</tt>; output an option
 *          only if its \c flags member has all these flags set
 *       -# <tt>NULL</tt>-terminated list of options whose metadata to format
 *       -# Whether to output all options (ignored, treated as \c true)
 */
PCMK__OUTPUT_ARGS("option-list", "const char *", "const char *", "const char *",
                  "uint32_t", "const pcmk__cluster_option_t *", "bool")
static int
option_list_xml(pcmk__output_t *out, va_list args)
{
    const char *name = va_arg(args, const char *);
    const char *desc_short = va_arg(args, const char *);
    const char *desc_long = va_arg(args, const char *);
    const uint32_t filter = va_arg(args, uint32_t);
    const pcmk__cluster_option_t *option_list =
        va_arg(args, pcmk__cluster_option_t *);

    CRM_ASSERT((out != NULL) && (name != NULL) && (desc_short != NULL)
               && (desc_long != NULL) && (option_list != NULL));

    pcmk__output_xml_create_parent(out, PCMK_XE_RESOURCE_AGENT,
                                   PCMK_XA_NAME, name,
                                   PCMK_XA_VERSION, PACEMAKER_VERSION,
                                   NULL);

    pcmk__output_create_xml_text_node(out, PCMK_XE_VERSION, PCMK_OCF_VERSION);
    add_desc_xml(out, true, desc_long);
    add_desc_xml(out, false, desc_short);

    pcmk__output_xml_create_parent(out, PCMK_XE_PARAMETERS, NULL);

    for (const pcmk__cluster_option_t *option = option_list;
         option->name != NULL; option++) {

        if (pcmk_all_flags_set(option->flags, filter)) {
            add_option_metadata_xml(out, option);
        }
    }

    pcmk__output_xml_pop_parent(out);
    pcmk__output_xml_pop_parent(out);
    return pcmk_rc_ok;
}

static pcmk__message_entry_t fmt_functions[] = {
    { "option-list", "default", option_list_default },
    { "option-list", "xml", option_list_xml },

    { NULL, NULL, NULL }
};

/*!
 * \internal
 * \brief Register the formatting functions for option lists
 *
 * \param[in,out] out  Output object
 */
void
pcmk__register_option_messages(pcmk__output_t *out) {
    pcmk__register_messages(out, fmt_functions);
}
