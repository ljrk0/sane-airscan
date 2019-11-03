/* AirScan (a.k.a. eSCL) backend for SANE
 *
 * Copyright (C) 2019 and up by Alexander Pevzner (pzz@apevzner.com)
 * See LICENSE for license terms and conditions
 *
 * Device capabilities
 */

#include "airscan.h"

#include <glib.h>
#include <strings.h>

/* Allocate devcaps_source
 */
static devcaps_source*
devcaps_source_new (void)
{
    devcaps_source *src = g_new0(devcaps_source, 1 );
    array_of_string_init(&src->sane_modes);
    array_of_word_init(&src->resolutions);
    return src;
}

/* Free devcaps_source
 */
static void
devcaps_source_free (devcaps_source *src)
{
    if (src != NULL) {
        array_of_string_cleanup(&src->sane_modes);
        array_of_word_cleanup(&src->resolutions);
        g_free(src);
    }
}

/* Initialize Device Capabilities
 */
void
devcaps_init (devcaps *caps)
{
    array_of_string_init(&caps->sane_sources);
}

/* Cleanup Device Capabilities
 */
void
devcaps_cleanup (devcaps *caps)
{
    array_of_string_cleanup(&caps->sane_sources);
    g_free((void*) caps->vendor);
    g_free((void*) caps->model);

    unsigned int i;
    for (i = 0; i < NUM_OPT_SOURCE; i ++) {
        devcaps_source_free(caps->src[i]);
    }
}

/* Reset Device Capabilities into initial state
 */
void
devcaps_reset (devcaps *caps)
{
    devcaps_cleanup(caps);
    memset(caps, 0, sizeof(*caps));
    devcaps_init(caps);
}

/* Choose appropriate scanner resolution
 */
SANE_Word
devcaps_source_choose_resolution(devcaps_source *src, SANE_Word wanted)
{
    if (src->flags & DEVCAPS_SOURCE_RES_DISCRETE) {
        SANE_Word res = src->resolutions[1];
        SANE_Word delta = (SANE_Word) labs(wanted - res);
        size_t i, end = array_of_word_len(&src->resolutions) + 1;

        for (i = 2; i < end; i ++) {
            SANE_Word res2 = src->resolutions[i];
            SANE_Word delta2 = (SANE_Word) labs(wanted - res2);

            if (delta2 <= delta) {
                res = res2;
                delta = delta2;
            }
        }

        return res;
    } else {
        return math_range_fit(&src->res_range, wanted);
    }
}

/* Parse color modes. Returns NULL on success, error string otherwise
 */
static const char*
devcaps_source_parse_color_modes (xml_iter *iter, devcaps_source *src)
{
    src->modes = 0;
    array_of_string_reset(&src->sane_modes);

    xml_iter_enter(iter);
    for (; !xml_iter_end(iter); xml_iter_next(iter)) {
        if(xml_iter_node_name_match(iter, "scan:ColorMode")) {
            const char *v = xml_iter_node_value(iter);
            if (!strcmp(v, "BlackAndWhite1")) {
                src->modes |= 1 << OPT_MODE_LINEART;
            } else if (!strcmp(v, "Grayscale8")) {
                src->modes |= 1 << OPT_MODE_GRAYSCALE;
            } else if (!strcmp(v, "RGB24")) {
                src->modes |= 1 << OPT_MODE_COLOR;
            }
        }
    }
    xml_iter_leave(iter);

    OPT_MODE opt_mode;
    for (opt_mode = (OPT_MODE) 0; opt_mode < NUM_OPT_MODE; opt_mode ++) {
        if ((src->modes & (1 << opt_mode)) != 0) {
            array_of_string_append(&src->sane_modes,
                    (SANE_String) opt_mode_to_sane(opt_mode));
        }
    }

    return NULL;
}

/* Parse document formats. Returns NULL on success, error string otherwise
 */
static const char*
devcaps_source_parse_document_formats (xml_iter *iter, devcaps_source *src)
{
    xml_iter_enter(iter);
    for (; !xml_iter_end(iter); xml_iter_next(iter)) {
        if(xml_iter_node_name_match(iter, "pwg:DocumentFormat") ||
           xml_iter_node_name_match(iter, "scan:DocumentFormatExt")) {
            const char *v = xml_iter_node_value(iter);
            if (!strcasecmp(v, "image/jpeg")) {
                src->flags |= DEVCAPS_SOURCE_FMT_JPEG;
            } else if (!strcasecmp(v, "image/png")) {
                src->flags |= DEVCAPS_SOURCE_FMT_PNG;
            } else if (!strcasecmp(v, "application/pdf")) {
                src->flags |= DEVCAPS_SOURCE_FMT_PDF;
            }
        }
    }
    xml_iter_leave(iter);

    return NULL;
}

/* Parse discrete resolutions.
 * Returns NULL on success, error string otherwise
 */
static const char*
devcaps_source_parse_discrete_resolutions (xml_iter *iter, devcaps_source *src)
{
    const char *err = NULL;

    array_of_word_reset(&src->resolutions);

    xml_iter_enter(iter);
    for (; err == NULL && !xml_iter_end(iter); xml_iter_next(iter)) {
        if (xml_iter_node_name_match(iter, "scan:DiscreteResolution")) {
            SANE_Word x = 0, y = 0;
            xml_iter_enter(iter);
            for (; err == NULL && !xml_iter_end(iter); xml_iter_next(iter)) {
                if (xml_iter_node_name_match(iter, "scan:XResolution")) {
                    err = xml_iter_node_value_uint(iter, &x);
                } else if (xml_iter_node_name_match(iter,
                        "scan:YResolution")) {
                    err = xml_iter_node_value_uint(iter, &y);
                }
            }
            xml_iter_leave(iter);

            if (x && y && x == y) {
                array_of_word_append(&src->resolutions, x);
            }
        }
    }
    xml_iter_leave(iter);

    if (array_of_word_len((&src->resolutions)) > 0) {
        src->flags |= DEVCAPS_SOURCE_RES_DISCRETE;
        array_of_word_sort(&src->resolutions);
    }

    return err;
}

/* Parse resolutions range
 * Returns NULL on success, error string otherwise
 */
static const char*
devcaps_source_parse_resolutions_range (xml_iter *iter, devcaps_source *src)
{
    const char *err = NULL;
    SANE_Range range_x = {0, 0, 0}, range_y = {0, 0, 0};

    xml_iter_enter(iter);
    for (; err == NULL && !xml_iter_end(iter); xml_iter_next(iter)) {
        SANE_Range *range = NULL;
        if (xml_iter_node_name_match(iter, "scan:XResolution")) {
            range = &range_x;
        } else if (xml_iter_node_name_match(iter, "scan:XResolution")) {
            range = &range_y;
        }

        if (range != NULL) {
            xml_iter_enter(iter);
            for (; err == NULL && !xml_iter_end(iter); xml_iter_next(iter)) {
                if (xml_iter_node_name_match(iter, "scan:Min")) {
                    err = xml_iter_node_value_uint(iter, &range->min);
                } else if (xml_iter_node_name_match(iter, "scan:Max")) {
                    err = xml_iter_node_value_uint(iter, &range->max);
                } else if (xml_iter_node_name_match(iter, "scan:Step")) {
                    err = xml_iter_node_value_uint(iter, &range->quant);
                }
            }
            xml_iter_leave(iter);
        }
    }
    xml_iter_leave(iter);

    if (range_x.min > range_x.max) {
        err = "Invalid scan:XResolution range";
        goto DONE;
    }

    if (range_y.min > range_y.max) {
        err = "Invalid scan:YResolution range";
        goto DONE;
    }

    /* If no quantization value, SANE uses 0, not 1
     */
    if (range_x.quant == 1) {
        range_x.quant = 0;
    }

    if (range_y.quant == 1) {
        range_y.quant = 0;
    }

    /* Try to merge x/y ranges */
    if (!math_range_merge(&src->res_range, &range_x, &range_y)) {
        err = "Incompatible scan:XResolution and scan:YResolution ranges";
        goto DONE;
    }

    src->flags |= DEVCAPS_SOURCE_RES_RANGE;

DONE:
    return err;
}

/* Parse supported resolutions.
 * Returns NULL on success, error string otherwise
 */
static const char*
devcaps_source_parse_resolutions (xml_iter *iter, devcaps_source *src)
{
    const char *err = NULL;

    xml_iter_enter(iter);
    for (; err == NULL && !xml_iter_end(iter); xml_iter_next(iter)) {
        if (xml_iter_node_name_match(iter, "scan:DiscreteResolutions")) {
            err = devcaps_source_parse_discrete_resolutions(iter, src);
        } else if (xml_iter_node_name_match(iter, "scan:ResolutionRange")) {
            err = devcaps_source_parse_resolutions_range(iter, src);
        }
    }
    xml_iter_leave(iter);

    /* Prefer discrete resolution, if both are provided */
    if (src->flags & DEVCAPS_SOURCE_RES_DISCRETE) {
        src->flags &= ~DEVCAPS_SOURCE_RES_RANGE;
    }

    if (!(src->flags & (DEVCAPS_SOURCE_RES_DISCRETE|DEVCAPS_SOURCE_RES_RANGE))){
        err = "Source resolutions are not defined";
    }

    return err;
}

/* Parse setting profiles (color modes, document formats etc).
 * Returns NULL on success, error string otherwise
 */
static const char*
devcaps_source_parse_setting_profiles (xml_iter *iter, devcaps_source *src)
{
    const char *err = NULL;

    xml_iter_enter(iter);
    for (; err == NULL && !xml_iter_end(iter); xml_iter_next(iter)) {
        if (xml_iter_node_name_match(iter, "scan:SettingProfile")) {
            xml_iter_enter(iter);
            for (; err == NULL && !xml_iter_end(iter); xml_iter_next(iter)) {
                if (xml_iter_node_name_match(iter, "scan:ColorModes")) {
                    err = devcaps_source_parse_color_modes(iter, src);
                } else if (xml_iter_node_name_match(iter,
                        "scan:DocumentFormats")) {
                    err = devcaps_source_parse_document_formats(iter, src);
                } else if (xml_iter_node_name_match(iter,
                        "scan:SupportedResolutions")) {
                    err = devcaps_source_parse_resolutions(iter, src);
                }
            }
            xml_iter_leave(iter);
        }
    }
    xml_iter_leave(iter);

    return err;
}


/* Parse source capabilities. Returns NULL on success, error string otherwise
 */
static const char*
devcaps_source_parse (xml_iter *iter, devcaps_source **out)
{
    devcaps_source *src = devcaps_source_new();
    const char *err = NULL;

    xml_iter_enter(iter);
    for (; err == NULL && !xml_iter_end(iter); xml_iter_next(iter)) {
        if(xml_iter_node_name_match(iter, "scan:MinWidth")) {
            err = xml_iter_node_value_uint(iter, &src->min_width);
        } else if (xml_iter_node_name_match(iter, "scan:MaxWidth")) {
            err = xml_iter_node_value_uint(iter, &src->max_width);
        } else if (xml_iter_node_name_match(iter, "scan:MinHeight")) {
            err = xml_iter_node_value_uint(iter, &src->min_height);
        } else if (xml_iter_node_name_match(iter, "scan:MaxHeight")) {
            err = xml_iter_node_value_uint(iter, &src->max_height);
        } else if (xml_iter_node_name_match(iter, "scan:SettingProfiles")) {
            err = devcaps_source_parse_setting_profiles(iter, src);
        }
    }
    xml_iter_leave(iter);

    if (err != NULL) {
        goto DONE;
    }

    if (src->max_width != 0 && src->max_height != 0 )
    {
        /* Validate window size */
        if (src->min_width >= src->max_width )
        {
            err = "Invalid scan:MinWidth or scan:MaxWidth";
            goto DONE;
        }

        if (src->min_height >= src->max_height)
        {
            err = "Invalid scan:MinHeight or scan:MaxHeight";
            goto DONE;
        }

        /* Recompute to millimeters */
        src->flags |= DEVCAPS_SOURCE_HAS_SIZE;
        src->br_x_range.min = SANE_FIX((double) src->min_width * 25.4 / 300);
        src->br_x_range.max = SANE_FIX((double) src->max_width * 25.4 / 300);
        src->br_y_range.min = SANE_FIX((double) src->min_height * 25.4 / 300);
        src->br_y_range.max = SANE_FIX((double) src->max_height * 25.4 / 300);

        src->tl_x_range = src->br_x_range;
        src->tl_x_range.min = 0;

        src->tl_y_range = src->br_y_range;
        src->tl_y_range.min = 0;
    }

DONE:
    if (err != NULL) {
        devcaps_source_free(src);
    } else {
        if (*out == NULL) {
            *out = src;
        } else {
            /* Duplicate detected. Ignored for now */
            devcaps_source_free(src);
        }
    }

    return err;
}

/* Parse device capabilities. devcaps structure must be initialized
 * before calling this function.
 *
 * Returns NULL if OK, error string otherwise
 */
const char*
devcaps_parse (devcaps *caps, xmlDoc *xml)
{
    const char *err = NULL;
    char       *model = NULL, *make_and_model = NULL;
    xml_iter   iter = XML_ITER_INIT;

    /* Parse capabilities XML */
    xml_iter_init(&iter, xmlDocGetRootElement(xml));
    if (!xml_iter_node_name_match(&iter, "scan:ScannerCapabilities")) {
        err = "XML: missed scan:ScannerCapabilities";
        goto DONE;
    }

    xml_iter_enter(&iter);
    for (; !xml_iter_end(&iter); xml_iter_next(&iter)) {
        if (xml_iter_node_name_match(&iter, "pwg:ModelName")) {
            g_free(model);
            model = g_strdup(xml_iter_node_value(&iter));
        } else if (xml_iter_node_name_match(&iter, "pwg:MakeAndModel")) {
            g_free(make_and_model);
            make_and_model = g_strdup(xml_iter_node_value(&iter));
        } else if (xml_iter_node_name_match(&iter, "scan:Platen")) {
            xml_iter_enter(&iter);
            if (xml_iter_node_name_match(&iter, "scan:PlatenInputCaps")) {
                err = devcaps_source_parse(&iter,
                    &caps->src[OPT_SOURCE_PLATEN]);
            }
            xml_iter_leave(&iter);
        } else if (xml_iter_node_name_match(&iter, "scan:Adf")) {
            xml_iter_enter(&iter);
            while (!xml_iter_end(&iter)) {
                if (xml_iter_node_name_match(&iter, "scan:AdfSimplexInputCaps")) {
                    err = devcaps_source_parse(&iter,
                        &caps->src[OPT_SOURCE_ADF_SIMPLEX]);
                } else if (xml_iter_node_name_match(&iter,
                        "scan:AdfDuplexInputCaps")) {
                    err = devcaps_source_parse(&iter,
                        &caps->src[OPT_SOURCE_ADF_DUPLEX]);
                }
                xml_iter_next(&iter);
            }
            xml_iter_leave(&iter);
        }

        if (err != NULL) {
            goto DONE;
        }
    }

    /* Save model, try to guess vendor */
    size_t model_len = model ? strlen(model) : 0;
    size_t make_and_model_len = make_and_model ? strlen(make_and_model) : 0;

    if (model_len && make_and_model_len > model_len &&
        g_str_has_suffix(make_and_model, model)) {

        caps->vendor = g_strndup(make_and_model,
                make_and_model_len - model_len);
        g_strchomp((char*) caps->vendor);
    }

    if (caps->vendor == NULL) {
        caps->vendor = g_strdup("Unknown");
    }

    if (model != NULL) {
        caps->model = model;
        model = NULL;
    } else if (make_and_model != NULL) {
        caps->model = make_and_model;
        make_and_model = NULL;
    }

    /* Update list of sources */
    OPT_SOURCE opt_src;

    array_of_string_reset(&caps->sane_sources);
    for (opt_src = (OPT_SOURCE) 0; opt_src < NUM_OPT_SOURCE; opt_src ++) {
        if (caps->src[opt_src] != NULL) {
            array_of_string_append(&caps->sane_sources,
                (SANE_String) opt_source_to_sane(opt_src));
        }
    }

DONE:
    if (err != NULL) {
        devcaps_reset(caps);
    }

    g_free(model);
    g_free(make_and_model);
    xml_iter_cleanup(&iter);

    return err;
}

/* Dump device capabilities, for debugging
 */
void
devcaps_dump (const char *name, devcaps *caps)
{
    int i, j;
    GString *buf = g_string_new(NULL);

    DBG_PROTO(name, "===== device capabilities =====");
    DBG_PROTO(name, "  Model: %s", caps->model);
    DBG_PROTO(name, "  Vendor: %s", caps->vendor);

    g_string_truncate(buf, 0);
    for (i = 0; caps->sane_sources[i] != NULL; i ++) {
        g_string_append_printf(buf, " \"%s\"", caps->sane_sources[i]);
    }
    DBG_PROTO(name, "  Sources:%s", buf->str);

    OPT_SOURCE opt_src;
    for (opt_src = (OPT_SOURCE) 0; opt_src < NUM_OPT_SOURCE; opt_src ++) {
        devcaps_source *src = caps->src[opt_src];
        if (src == NULL) {
            continue;
        }

        DBG_PROTO(name, "  %s:", opt_source_to_sane(opt_src));
        DBG_PROTO(name, "    Min Width/Height: %d/%d", src->min_width, src->min_height);
        DBG_PROTO(name, "    Max Width/Height: %d/%d", src->max_width, src->max_height);

        if (src->flags & DEVCAPS_SOURCE_RES_DISCRETE) {
            g_string_truncate(buf, 0);
            for (j = 0; j < (int) array_of_word_len(&src->resolutions); j ++) {
                g_string_append_printf(buf, " %d", src->resolutions[j+1]);
            }
            DBG_PROTO(name, "    Resolutions: %s", buf->str);
        }

        g_string_truncate(buf, 0);
        for (i = 0; src->sane_modes[i] != NULL; i ++) {
            g_string_append_printf(buf, " \"%s\"", src->sane_modes[i]);
        }
        DBG_PROTO(name, "    Modes:%s", buf->str);

    }

    g_string_free(buf, TRUE);
}

/* vim:ts=8:sw=4:et
 */