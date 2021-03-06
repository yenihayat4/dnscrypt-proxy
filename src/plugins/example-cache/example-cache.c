
#include <dnscrypt/plugin.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

DCPLUGIN_MAIN(__FILE__);

#define DEFAULT_CACHE_ENTRIES_MAX 50
#define DEFAULT_MIN_TTL 60
#define MAX_TTL 86400

#define DNS_MAX_HOSTNAME_LEN 256U

#ifndef putc_unlocked
# define putc_unlocked(c, stream) putc((c), (stream))
#endif

typedef struct CacheEntry_ {
    struct CacheEntry_ *next;
    uint8_t            *response;
    uint16_t            response_len;
    char                qname[DNS_MAX_HOSTNAME_LEN];
    uint16_t            qtype;
    time_t              deadline;
} CacheEntry;

typedef struct Cache_ {
    CacheEntry *cache_entries;
    size_t      cache_entries_max;
    time_t      now;
    time_t      min_ttl;
} Cache;

const char *
dcplugin_description(DCPlugin * const dcplugin)
{
    return "A toy DNS cache";
}

const char *
dcplugin_long_description(DCPlugin * const dcplugin)
{
    return
        "This plugin implements a very basic DNS cache, designed to avoid\n"
        "sending the same queries multiple times in a row.\n"
        "\n"
        "Usage:\n"
        "\n"
        "# dnscrypt-proxy --plugin \\\n"
        "  libdcplugin_example_cache\n";
}

int
dcplugin_init(DCPlugin * const dcplugin, int argc, char *argv[])
{
    Cache *cache;

    if ((cache = calloc((size_t) 1U, sizeof *cache)) == NULL) {
        return -1;
    }
    cache->cache_entries = NULL;
    cache->cache_entries_max = DEFAULT_CACHE_ENTRIES_MAX;
    cache->min_ttl = DEFAULT_MIN_TTL;
    cache->now = (time_t) 0;
    dcplugin_set_user_data(dcplugin, cache);

    return 0;
}

int
dcplugin_destroy(DCPlugin * const dcplugin)
{
    Cache      *cache = dcplugin_get_user_data(dcplugin);
    CacheEntry *cache_entry;
    CacheEntry *next;

    if (cache == NULL) {
        return 0;
    }
    cache_entry = cache->cache_entries;
    while (cache_entry != NULL) {
        next = cache_entry->next;
        free(cache_entry->response);
        cache_entry->response = NULL;
        cache_entry = next;
    }
    cache->cache_entries = NULL;
    free(cache);

    return 0;
}

static void
name_tolower(uint8_t *name)
{
    while (*name != 0) {
        *name = (uint8_t) tolower(*name);
        name++;
    }
}

static int
skip_name(const uint8_t * const dns_packet, const size_t dns_packet_len,
          size_t * const offset_p)
{
    size_t  offset = *offset_p;
    size_t  name_len = (size_t) 0U;
    uint8_t label_len;

    if (dns_packet_len < (size_t) 1U ||
        offset >= dns_packet_len - (size_t) 1U) {
        return -1;
    }
    for (;;) {
        label_len = dns_packet[offset];
        if ((label_len & 0xc0) == 0xc0) {
            if (2U > dns_packet_len - offset) {
                return -1;
            }
            offset += 2U;
            break;
        }
        if (label_len >= dns_packet_len - offset - 1U) {
            return -1;
        }
        name_len += (size_t) label_len + (size_t) 1U;
        if (name_len > DNS_MAX_HOSTNAME_LEN) {
            return -1;
        }
        offset += label_len + 1U;
        if (label_len == 0U) {
            break;
        }
    }
    if (offset >= dns_packet_len) {
        return -1;
    }
    *offset_p = offset;

    return 0;
}

static int
next_rr(const uint8_t * const dns_packet, const size_t dns_packet_len,
        const _Bool is_question, size_t * const name_len_p,
        size_t * const offset_p, uint16_t * const qtype_p,
        uint16_t * const qclass_p, uint32_t * const ttl_p)
{
    size_t   offset = *offset_p;
    uint16_t rdlen;

    if (skip_name(dns_packet, dns_packet_len, &offset) != 0) {
        return -1;
    }
    assert((offset - *offset_p) <= 0xffff);
    if (name_len_p != NULL) {
        *name_len_p = (size_t) (offset - *offset_p);
    }
    if ((is_question ? 6 : 10) > dns_packet_len - offset) {
        return -1;
    }
    if (qtype_p != NULL) {
        *qtype_p = (dns_packet[offset] << 8) | dns_packet[offset + 1];
    }
    if (qclass_p != NULL) {
        *qclass_p = (dns_packet[offset + 2] << 8) | dns_packet[offset + 3];
    }
    offset += 4;
    if (is_question == 0) {
        if (ttl_p != NULL) {
            *ttl_p = ((uint32_t) dns_packet[offset] << 24) |
                     ((uint32_t) dns_packet[offset + 1] << 16) |
                     ((uint32_t) dns_packet[offset + 2] << 8) |
                     ((uint32_t) dns_packet[offset + 3]);
        }
        offset += 4;
        rdlen = (dns_packet[offset] << 8) | dns_packet[offset + 1];
        offset += 2;
        if (rdlen > dns_packet_len - offset) {
            return -1;
        }
        offset += rdlen;
    }
    *offset_p = offset;

    return 0;
}

DCPluginSyncFilterResult
dcplugin_sync_pre_filter(DCPlugin *dcplugin, DCPluginDNSPacket *dcp_packet)
{
    uint8_t     qname[DNS_MAX_HOSTNAME_LEN];
    Cache      *cache = dcplugin_get_user_data(dcplugin);
    CacheEntry *scanned_cache_entry;
    uint8_t    *wire_qname;
    uint8_t    *wire_data = dcplugin_get_wire_data(dcp_packet);
    size_t      wire_data_len = dcplugin_get_wire_data_len(dcp_packet);
    size_t      i = (size_t) 12U;
    size_t      qname_len;
    uint16_t    qtype;
    uint16_t    qclass;
    uint16_t    tid;

    if (wire_data_len < 15U || wire_data[4] != 0U || wire_data[5] != 1U ||
        wire_data[10] != 0 || wire_data[11] > 1) {
        return DCP_SYNC_FILTER_RESULT_ERROR;
    }
    wire_qname = &wire_data[i];
    if (next_rr(wire_data, wire_data_len, 1, &qname_len, &i,
                &qtype, &qclass, NULL) != 0) {
        return DCP_SYNC_FILTER_RESULT_ERROR;
    }
    if (qclass != 1) {
        return DCP_SYNC_FILTER_RESULT_OK;
    }
    assert(qname_len <= sizeof qname);
    memcpy(qname, wire_qname, qname_len);
    name_tolower(qname);
    if (wire_data[11] == 1) {
        uint16_t opt_type;
        uint32_t opt_ttl;

        if (next_rr(wire_data, wire_data_len, 0, NULL, &i,
                    &opt_type, NULL, &opt_ttl) != 0) {
            return DCP_SYNC_FILTER_RESULT_ERROR;
        }
        if (opt_type != 41) {
            return DCP_SYNC_FILTER_RESULT_OK;
        }
        if ((opt_ttl & 0x8000) == 0x8000) {
            if (qname_len >= 2) {
                qname[qname_len - 2] = (uint8_t) toupper(qname[qname_len - 2]);
                wire_qname[qname_len - 2] = qname[qname_len - 2];
            }
        }
    }
    scanned_cache_entry = cache->cache_entries;
    while (scanned_cache_entry != NULL) {
        if (memcmp(scanned_cache_entry->qname, qname, qname_len) == 0 &&
            scanned_cache_entry->qtype == qtype) {
            break;
        }
        scanned_cache_entry = scanned_cache_entry->next;
    }
    time(&cache->now);
    if (scanned_cache_entry != NULL &&
        scanned_cache_entry->response_len <=
        dcplugin_get_wire_data_max_len(dcp_packet) &&
        cache->now < scanned_cache_entry->deadline) {
        size_t   aname_i;
        size_t   aname_len;
        size_t   ttl_i;
        uint32_t ttl;

        tid = (wire_data[0] << 8) | wire_data[1];
        memcpy(qname, wire_qname, qname_len);
        dcplugin_set_wire_data(dcp_packet, scanned_cache_entry->response,
                               scanned_cache_entry->response_len);
        wire_data = dcplugin_get_wire_data(dcp_packet);
        wire_data[0] = (uint8_t) (tid >> 8);
        wire_data[1] = (uint8_t) tid;
        memcpy(&wire_data[12], qname, qname_len);
        i = 12;
        wire_data_len = scanned_cache_entry->response_len;
        if (next_rr(wire_data, wire_data_len, 1, NULL, &i,
                    NULL, NULL, NULL) != 0) {
            return DCP_SYNC_FILTER_RESULT_ERROR;
        }
        ttl = scanned_cache_entry->deadline - cache->now;
        aname_i = i;
        while (next_rr(wire_data, wire_data_len, 0, &aname_len, &i,
                       NULL, NULL, NULL) == 0) {
            ttl_i = aname_i + aname_len + 4;
            if (4 > wire_data_len - ttl_i) {
                return DCP_SYNC_FILTER_RESULT_ERROR;
            }
            wire_data[ttl_i] = (uint8_t) (ttl >> 24);
            wire_data[ttl_i + 1] = (uint8_t) (ttl >> 16);
            wire_data[ttl_i + 2] = (uint8_t) (ttl >> 8);
            wire_data[ttl_i + 3] = (uint8_t) ttl;
            aname_i = i;
        }
        return DCP_SYNC_FILTER_RESULT_DIRECT;
    }
    return DCP_SYNC_FILTER_RESULT_OK;
}

DCPluginSyncFilterResult
dcplugin_sync_post_filter(DCPlugin *dcplugin, DCPluginDNSPacket *dcp_packet)
{
    Cache      *cache = dcplugin_get_user_data(dcplugin);
    CacheEntry *scanned_cache_entry;
    CacheEntry *last_cache_entry;
    CacheEntry *last_cache_entry_parent;
    uint8_t    *wire_data = dcplugin_get_wire_data(dcp_packet);
    uint8_t    *wire_qname;
    uint8_t    *response_tmp;
    size_t      wire_data_len = dcplugin_get_wire_data_len(dcp_packet);
    size_t      cache_entries_count;
    size_t      i = (size_t) 12U;
    size_t      qname_len;
    uint32_t    ttl;
    uint32_t    min_ttl;
    uint16_t    qtype;
    uint16_t    qclass;
    uint16_t    tid;
    uint8_t     c;

    if (wire_data_len < 15U || wire_data[4] != 0U || wire_data[5] != 1U) {
        return DCP_SYNC_FILTER_RESULT_ERROR;
    }
    if ((wire_data[2] & 2) != 0) {
        return DCP_SYNC_FILTER_RESULT_OK;
    }
    if ((wire_data[3] & 0xf) != 0 && (wire_data[3] & 0xf) != 3) {
        return DCP_SYNC_FILTER_RESULT_OK;
    }
    wire_qname = &wire_data[i];
    if (next_rr(wire_data, wire_data_len, 1, &qname_len, &i,
                &qtype, &qclass, NULL) != 0) {
        return DCP_SYNC_FILTER_RESULT_ERROR;
    }
    if (qclass != 1) {
        return DCP_SYNC_FILTER_RESULT_OK;
    }
    min_ttl = MAX_TTL;
    while (next_rr(wire_data, wire_data_len, 0, NULL, &i,
                   NULL, NULL, &ttl) == 0) {
        if (ttl < min_ttl) {
            min_ttl = ttl;
        }
    }
    if (ttl < cache->min_ttl) {
        ttl = cache->min_ttl;
    }
    scanned_cache_entry = cache->cache_entries;
    last_cache_entry_parent = last_cache_entry = NULL;
    cache_entries_count = 0;
    while (scanned_cache_entry != NULL) {
        cache_entries_count++;
        last_cache_entry_parent = last_cache_entry;
        last_cache_entry = scanned_cache_entry;
        if (memcmp(scanned_cache_entry->qname, wire_qname, qname_len) == 0 &&
            scanned_cache_entry->qtype == qtype) {
            break;
        }
        scanned_cache_entry = scanned_cache_entry->next;
    }
    if (scanned_cache_entry != NULL) {
        if (wire_data_len > scanned_cache_entry->response_len) {
            if ((response_tmp = realloc(scanned_cache_entry->response,
                                        wire_data_len)) == NULL) {
                return DCP_SYNC_FILTER_RESULT_OK;
            }
            scanned_cache_entry->response = response_tmp;
        }
        memcpy(scanned_cache_entry->response, wire_data, wire_data_len);
        scanned_cache_entry->response_len = wire_data_len;
        scanned_cache_entry->deadline = cache->now + ttl;
        if (last_cache_entry_parent != NULL) {
            assert(last_cache_entry_parent->next = scanned_cache_entry);
            last_cache_entry_parent->next = NULL;
            scanned_cache_entry->next = cache->cache_entries;
            cache->cache_entries = scanned_cache_entry;
        }
    } else {
        CacheEntry *cache_entry;

        if (cache_entries_count >= cache->cache_entries_max &&
            last_cache_entry_parent != NULL && last_cache_entry != NULL) {
            free(last_cache_entry->response);
            last_cache_entry->response = NULL;
            assert(last_cache_entry->next == NULL);
            assert(last_cache_entry_parent->next == last_cache_entry);
            free(last_cache_entry);
            last_cache_entry_parent->next = NULL;
        }
        if ((cache_entry = calloc((size_t) 1U, sizeof *cache_entry)) == NULL) {
            return DCP_SYNC_FILTER_RESULT_OK;
        }
        memcpy(cache_entry->qname, wire_qname, qname_len);
        cache_entry->qtype = qtype;
        if ((cache_entry->response = malloc(wire_data_len)) == NULL) {
            free(cache_entry);
            return DCP_SYNC_FILTER_RESULT_OK;
        }
        memcpy(cache_entry->response, wire_data, wire_data_len);
        cache_entry->response_len = wire_data_len;
        cache_entry->deadline = cache->now + ttl;
        cache_entry->next = cache->cache_entries;
        cache->cache_entries = cache_entry;
    }
    return DCP_SYNC_FILTER_RESULT_OK;
}
