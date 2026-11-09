#define _POSIX_C_SOURCE 200809L

#include "TsStats.h"

#include <moq/endpoint.h>
#include <moq/media_receiver.h>
#include <moq/msf.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stopping;

static void on_signal(int signum) {
    (void)signum;
    stopping = 1;
}

typedef struct options {
    const char *url;
    const char *namespace_name;
    const char *track_name;
    const char *out_path;
    const char *stats_path;
    unsigned duration;
    unsigned draft;   /* 22 (the default) or 18 */
    bool insecure;
    bool fail_on_gap;
} options_t;

typedef struct candidate {
    moq_media_track_t *handle;
    const moq_media_track_desc_t *desc;
    const moq_msf_mpeg2ts_t *ts;
} candidate_t;

static void usage(FILE *out) {
    fputs("moq2ts-receiver --url moqt://host:port --namespace live/name --out capture.ts\n"
          "  [--track NAME] [--stats-json FILE] [--duration SECONDS] [--draft 22|18]\n"
          "  [--insecure] [--fail-on-gap]\n"
          "  https:// URLs use WebTransport when moq5 was built with that backend.\n"
          "  Stats go to stderr; --out - sends source packets to stdout.\n", out);
}

static bool parse_options(int argc, char **argv, options_t *o) {
    memset(o, 0, sizeof(*o));
    o->draft = 22;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            exit(0);
        } else if (!strcmp(argv[i], "--insecure")) {
            o->insecure = true;
        } else if (!strcmp(argv[i], "--fail-on-gap")) {
            o->fail_on_gap = true;
        } else if (i + 1 < argc && !strcmp(argv[i], "--url")) {
            o->url = argv[++i];
        } else if (i + 1 < argc && !strcmp(argv[i], "--namespace")) {
            o->namespace_name = argv[++i];
        } else if (i + 1 < argc && !strcmp(argv[i], "--track")) {
            o->track_name = argv[++i];
        } else if (i + 1 < argc && !strcmp(argv[i], "--out")) {
            o->out_path = argv[++i];
        } else if (i + 1 < argc && !strcmp(argv[i], "--stats-json")) {
            o->stats_path = argv[++i];
        } else if (i + 1 < argc && !strcmp(argv[i], "--draft")) {
            const char *draft = argv[++i];
            if (!strcmp(draft, "22")) o->draft = 22;
            else if (!strcmp(draft, "18")) o->draft = 18;
            else return false;
        } else if (i + 1 < argc && !strcmp(argv[i], "--duration")) {
            char *end = NULL;
            unsigned long n = strtoul(argv[++i], &end, 10);
            if (!argv[i][0] || *end || n == 0 || n > 86400) return false;
            o->duration = (unsigned)n;
        } else {
            return false;
        }
    }
    if (!o->url || !o->namespace_name || !o->out_path || !o->url[0] ||
        !o->namespace_name[0] || !o->out_path[0]) return false;
    if (strncmp(o->url, "moqt://", 7) && strncmp(o->url, "https://", 8)) return false;
    return true;
}

static bool split_namespace(char *copy, moq_bytes_t *parts, size_t *count) {
    size_t n = 0;
    char *part = copy;
    while (1) {
        if (!*part || n == 32) return false;
        char *slash = strchr(part, '/');
        if (slash) *slash = 0;
        if (!*part) return false;
        parts[n].data = (const uint8_t *)part;
        parts[n++].len = strlen(part);
        if (!slash) break;
        part = slash + 1;
    }
    *count = n;
    return true;
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static bool bytes_equal(moq_bytes_t bytes, const char *name) {
    size_t len = strlen(name);
    return bytes.len == len && memcmp(bytes.data, name, len) == 0;
}

static void print_safe(moq_bytes_t bytes) {
    size_t len = bytes.len < 128 ? bytes.len : 128;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = bytes.data[i];
        fputc(c >= 0x20 && c <= 0x7e ? c : '?', stderr);
    }
    if (bytes.len > len) fputs("...", stderr);
}

static void readout(const candidate_t *track, uint64_t objects, uint64_t bytes,
                    uint64_t groups, const receiver_ts_stats_t *ts,
                    const moq_media_receiver_stats_t *rx, double elapsed,
                    uint64_t prev_bytes) {
    fputs("[rx] track=", stderr);
    print_safe(track->desc->name);
    fprintf(stderr, " objects=%llu groups=%llu bytes=%llu rate=%.1f kbit/s"
            " ts_packets=%llu sync=%llu malformed=%llu tei=%llu cc=%llu moqt_gaps=%llu"
            " drops=%llu parse_drops=%llu reorder_refused=%llu\n",
            (unsigned long long)objects, (unsigned long long)groups,
            (unsigned long long)bytes,
            elapsed > 0 ? (double)(bytes - prev_bytes) * 8.0 / elapsed / 1000.0 : 0,
            (unsigned long long)ts->packets, (unsigned long long)ts->sync_errors,
            (unsigned long long)ts->malformed_packets,
            (unsigned long long)ts->transport_errors,
            (unsigned long long)ts->continuity_errors,
            (unsigned long long)rx->gaps_reported,
            (unsigned long long)rx->objects_dropped,
            (unsigned long long)rx->parse_drops,
            (unsigned long long)rx->reorder_refused);
}

static bool write_stats(const char *path, uint64_t objects, uint64_t bytes,
                        uint64_t groups, const receiver_ts_stats_t *ts,
                        const moq_media_receiver_stats_t *rx) {
    if (!path) return true;
    FILE *out = fopen(path, "wb");
    if (!out) return false;
    int n = fprintf(out,
        "{\"objects\":%llu,\"groups\":%llu,\"bytes\":%llu,"
        "\"ts_packets\":%llu,\"sync_errors\":%llu,"
        "\"malformed_packets\":%llu,\"transport_errors\":%llu,"
        "\"continuity_errors\":%llu,\"moqt_gaps\":%llu,"
        "\"objects_dropped\":%llu,\"parse_drops\":%llu,"
        "\"reorder_refused\":%llu}\n",
        (unsigned long long)objects, (unsigned long long)groups,
        (unsigned long long)bytes, (unsigned long long)ts->packets,
        (unsigned long long)ts->sync_errors,
        (unsigned long long)ts->malformed_packets,
        (unsigned long long)ts->transport_errors,
        (unsigned long long)ts->continuity_errors,
        (unsigned long long)rx->gaps_reported,
        (unsigned long long)rx->objects_dropped,
        (unsigned long long)rx->parse_drops,
        (unsigned long long)rx->reorder_refused);
    bool ok = n > 0 && !ferror(out);
    if (fclose(out) != 0) ok = false;
    return ok;
}

int main(int argc, char **argv) {
    options_t opt;
    if (!parse_options(argc, argv, &opt)) {
        usage(stderr);
        return 2;
    }
    char *ns = strdup(opt.namespace_name);
    moq_bytes_t parts[32];
    size_t part_count = 0;
    if (!ns || !split_namespace(ns, parts, &part_count)) {
        fprintf(stderr, "invalid namespace\n");
        free(ns);
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    moq_endpoint_cfg_t ec;
    moq_endpoint_cfg_init_sized(&ec, sizeof(ec));
    ec.url.data = (const uint8_t *)opt.url;
    ec.url.len = strlen(opt.url);
    ec.backend = MOQ_TRANSPORT_BACKEND_AUTO;
    ec.insecure_skip_verify = opt.insecure;
    const moq_version_t draft = opt.draft == 18 ? MOQ_VERSION_DRAFT_18 : MOQ_VERSION_DRAFT_22;
    ec.versions.struct_size = sizeof(ec.versions);
    ec.versions.policy = MOQ_VERSION_POLICY_EXACT;
    ec.versions.versions = &draft;
    ec.versions.version_count = 1;
    moq_endpoint_t *ep = NULL;
    moq_result_t rc = moq_endpoint_connect(&ec, &ep);
    if (rc != MOQ_OK) {
        fprintf(stderr, "endpoint connect failed: %s\n", moq_strerror(rc));
        free(ns);
        return 1;
    }

    moq_media_receiver_cfg_t cfg;
    moq_media_receiver_cfg_init_flow_control_sized(&cfg, sizeof(cfg));
    cfg.namespace_.parts = parts;
    cfg.namespace_.count = part_count;
    cfg.auto_subscribe = false;
    cfg.mpeg2ts_strip_prefix = false;
    cfg.mpeg2ts_join_psi = false;
    moq_media_receiver_t *rx = NULL;
    rc = moq_media_receiver_attach(ep, &cfg, &rx);
    if (rc != MOQ_OK) {
        fprintf(stderr, "receiver attach failed: %s\n", moq_strerror(rc));
        moq_endpoint_stop(ep);
        moq_endpoint_destroy(ep);
        free(ns);
        return 1;
    }

    candidate_t found[64];
    size_t found_count = 0;
    candidate_t *selected = NULL;
    bool catalog_ready = false, fatal = false, ended = false;
    FILE *out = NULL;
    receiver_ts_stats_t ts;
    receiver_ts_stats_init(&ts);
    uint64_t objects = 0, bytes = 0, groups = 0, prev_bytes = 0;
    uint64_t last_group = 0;
    uint64_t last_object = 0;
    uint64_t duplicates = 0;
    uint64_t missing_objects = 0;
    double start = monotonic_seconds(), last_report = start;

    while (!stopping && !fatal && !ended) {
        moq_media_track_event_t ev;
        while (moq_media_receiver_poll_track(rx, &ev, sizeof(ev)) == MOQ_OK) {
            if (ev.kind == MOQ_MEDIA_TRACK_ADDED && ev.desc &&
                ev.desc->info.packaging == MOQ_MEDIA_PACKAGING_MPEG2TS) {
                const moq_msf_mpeg2ts_t *ext =
                    moq_msf_ext_mpeg2ts(ev.desc->extensions, ev.desc->extension_count);
                if (!ext || !ext->has_packet_size ||
                    (ext->packet_size != 188 && ext->packet_size != 192) ||
                    ext->mode == MOQ_MSF_MPEG2TS_MODE_ES_PACKETS) continue;
                if (found_count == 64) {
                    fputs("too many TS tracks\n", stderr);
                    fatal = true;
                    break;
                }
                found[found_count++] = (candidate_t){ev.track, ev.desc, ext};
            } else if (ev.kind == MOQ_MEDIA_CATALOG_READY) {
                catalog_ready = true;
                size_t matches = 0;
                for (size_t i = 0; i < found_count; i++) {
                    if (opt.track_name && !bytes_equal(found[i].desc->name, opt.track_name)) continue;
                    if (!opt.track_name && found_count > 1 &&
                        found[i].ts->mode != MOQ_MSF_MPEG2TS_MODE_UNMODIFIED_MULTIPLEX) continue;
                    selected = &found[i];
                    matches++;
                }
                if (matches != 1) {
                    fprintf(stderr, "catalog has %zu matching TS tracks; pass --track\n", matches);
                    fatal = true;
                    break;
                }
                fprintf(stderr, "[rx] draft=%u selected=", (unsigned)moq_endpoint_negotiated_version(ep));
                print_safe(selected->desc->name);
                fprintf(stderr, " packet_size=%u\n", selected->ts->packet_size);
                out = !strcmp(opt.out_path, "-") ? stdout : fopen(opt.out_path, "wb");
                moq_result_t sub_rc = out ? moq_media_receiver_subscribe_track(rx, selected->handle, NULL)
                                          : MOQ_ERR_INTERNAL;
                if (!out || sub_rc != MOQ_OK) {
                    fprintf(stderr, "output open or subscribe failed: %s\n", moq_strerror(sub_rc));
                    fatal = true;
                    break;
                }
            } else if ((ev.kind == MOQ_MEDIA_TRACK_ENDED || ev.kind == MOQ_MEDIA_TRACK_REMOVED) &&
                       selected && ev.track == selected->handle) {
                /* A catalog that drops the track, such as the isComplete
                 * catalog that ends a live broadcast (MSF Section 11.3),
                 * ends it too. */
                fputs("[rx] track ended\n", stderr);
                ended = true;
            } else if ((ev.kind == MOQ_MEDIA_TRACK_LOCAL_FAILURE ||
                        ev.kind == MOQ_MEDIA_TRACK_DELIVERY_LOCAL_FAILURE) &&
                       selected && ev.track == selected->handle) {
                fputs("selected track failed locally\n", stderr);
                fatal = true;
            }
        }
        /* Objects queued before the end are still written; the loop exits
         * after them. */
        if (fatal) break;
        moq_media_object_t obj;
        while (moq_media_receiver_poll_object(rx, &obj, sizeof(obj)) == MOQ_OK) {
            bool duplicate = false;
            if (selected && obj.track == selected->handle && obj.status == MOQ_OBJECT_NORMAL && objects > 0) {
                /* An object at or before the last one written was already
                 * delivered (the same group and object again). It is not
                 * written, so the output stays the source and the continuity
                 * counters do not count the repeat as errors. */
                duplicate = obj.group_id < last_group ||
                            (obj.group_id == last_group && obj.object_id <= last_object);
                if (duplicate) {
                    duplicates++;
                    fprintf(stderr, "[rx] duplicate object group=%llu object=%llu dropped\n",
                            (unsigned long long)obj.group_id, (unsigned long long)obj.object_id);
                }
            }
            if (selected && obj.track == selected->handle && obj.status == MOQ_OBJECT_NORMAL && !duplicate) {
                /* A gap is a missing object: the next group, or the next object
                 * of the group, is not the one that follows the last. */
                bool missing = objects > 0 &&
                               (obj.group_id == last_group ? obj.object_id != last_object + 1
                                                           : obj.group_id != last_group + 1 || obj.object_id != 0);
                uint32_t size = selected->ts->packet_size;
                bool valid = obj.payload.len > 0 && obj.payload.len % size == 0;
                for (size_t pos = 0; valid && pos < obj.payload.len; pos += size)
                    valid = receiver_ts_stats_add(&ts, obj.payload.data + pos, size);
                if (!valid) {
                    fputs("invalid MPEG-TS object\n", stderr);
                    fatal = true;
                } else if (fwrite(obj.payload.data, 1, obj.payload.len, out) != obj.payload.len) {
                    fputs("output write failed\n", stderr);
                    fatal = true;
                } else {
                    objects++;
                    bytes += obj.payload.len;
                    if (objects == 1)
                        fprintf(stderr, "[rx] first object group=%llu object=%llu\n",
                                (unsigned long long)obj.group_id, (unsigned long long)obj.object_id);
                    if (groups == 0 || obj.group_id != last_group) {
                        groups++;
                        last_group = obj.group_id;
                    }
                    last_object = obj.object_id;
                    if (missing) {
                        missing_objects++;
                        fprintf(stderr, "[rx] MOQT gap before group=%llu object=%llu\n",
                                (unsigned long long)obj.group_id,
                                (unsigned long long)obj.object_id);
                    }
                }
            }
            moq_media_object_cleanup(&obj);
            if (fatal) break;
        }
        double now = monotonic_seconds();
        if (selected && now - last_report >= 1.0) {
            moq_media_receiver_stats_t stats;
            if (moq_media_receiver_get_stats(rx, &stats, sizeof(stats)) == MOQ_OK)
                readout(selected, objects, bytes, groups, &ts, &stats,
                        now - last_report, prev_bytes);
            prev_bytes = bytes;
            last_report = now;
        }
        if (opt.duration && now - start >= opt.duration) break;
        if (moq_media_receiver_is_fatal(rx)) {
            fprintf(stderr, "receiver fatal code=%llu\n",
                    (unsigned long long)moq_media_receiver_fatal_code(rx));
            fatal = true;
            break;
        }
        if (moq_media_receiver_is_closed(rx)) break;
        moq_media_receiver_wait(rx, 100000);
    }

    moq_media_receiver_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    moq_media_receiver_get_stats(rx, &stats, sizeof(stats));
    if (!catalog_ready) {
        fprintf(stderr, "[rx] no MSF catalog received; negotiated_draft=%u closed=%d fatal=%d\n",
                (unsigned)moq_endpoint_negotiated_version(ep),
                moq_media_receiver_is_closed(rx), moq_media_receiver_is_fatal(rx));
    }
    if (out && out != stdout && fclose(out) != 0) fatal = true;
    if (out == stdout && fflush(out) != 0) fatal = true;
    if (!write_stats(opt.stats_path, objects, bytes, groups, &ts, &stats)) {
        fputs("stats write failed\n", stderr);
        fatal = true;
    }
    if (selected) readout(selected, objects, bytes, groups, &ts, &stats,
                          monotonic_seconds() - last_report, prev_bytes);
    if (objects > 0)
        fprintf(stderr, "[rx] last object group=%llu object=%llu\n",
                (unsigned long long)last_group, (unsigned long long)last_object);
    if (opt.fail_on_gap && (missing_objects || duplicates || stats.gaps_reported || stats.objects_dropped ||
                            stats.parse_drops || stats.reorder_refused ||
                            ts.sync_errors || ts.malformed_packets ||
                            ts.transport_errors ||
                            ts.continuity_errors)) fatal = true;
    moq_media_receiver_destroy(rx);
    moq_endpoint_stop(ep);
    moq_endpoint_destroy(ep);
    free(ns);
    if (!catalog_ready || objects == 0) fatal = true;
    return fatal ? 1 : 0;
}
