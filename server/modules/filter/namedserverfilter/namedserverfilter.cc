/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file namedserverfilter.cc - a very simple regular expression based filter
 * that routes to a named server or server type if a regular expression match
 * is found.
 * @verbatim
 *
 * A simple regular expression based query routing filter.
 * Two parameters should be defined in the filter configuration
 *      match=<regular expression>
 *      server=<server to route statement to>
 * Two optional parameters
 *      source=<source address to limit filter>
 *      user=<username to limit filter>
 *
 * @endverbatim
 */

#define MXS_MODULE_NAME "namedserverfilter"

#include <maxscale/cppdefs.hh>

#include <stdio.h>
#include <string>
#include <string.h>
#include <vector>

#include <maxscale/alloc.h>
#include <maxscale/filter.h>
#include <maxscale/hint.h>
#include <maxscale/log_manager.h>
#include <maxscale/modinfo.h>
#include <maxscale/modutil.h>
#include <maxscale/pcre2.hh>
#include <maxscale/utils.h>

using std::string;

class RegexHintInst;
struct RegexHintSess_t;
struct RegexToServers;

typedef std::vector<string> StringArray;
typedef std::vector<RegexToServers> MappingArray;

typedef struct source_host
{
    const char *address;
    struct sockaddr_in ipv4;
    int netmask;
} REGEXHINT_SOURCE_HOST;

/**
 * Instance structure
 */
class RegexHintInst : public MXS_FILTER
{
private:
    string m_user; /* User name to restrict matches */
    REGEXHINT_SOURCE_HOST *m_source; /* Source address to restrict matches */
    MappingArray m_mapping; /* Regular expression to serverlist mapping */
    int m_ovector_size; /* Given to pcre2_match_data_create() */

    /* Total statements diverted statistics. Unreliable due to non-locked but
     * shared access. */
    volatile unsigned int m_total_diverted;
    volatile unsigned int m_total_undiverted;

    int check_source_host(const char *remote, const struct sockaddr_in *ipv4);

public:
    RegexHintInst(string user, REGEXHINT_SOURCE_HOST* source, MappingArray& map,
                  int ovector_size);
    ~RegexHintInst();
    RegexHintSess_t* newSession(MXS_SESSION *session);
    int routeQuery(RegexHintSess_t* session, GWBUF *queue);
    void diagnostic(RegexHintSess_t* session, DCB *dcb);
    int find_servers(RegexHintSess_t* my_session, StringArray& servers, char* sql,
                     int sql_len);
};

/**
 * The session structure for this regexhint filter
 */
typedef struct RegexHintSess_t
{
    MXS_DOWNSTREAM down; /* The downstream filter */
    int n_diverted; /* No. of statements diverted */
    int n_undiverted; /* No. of statements not diverted */
    int active; /* Is filter active */
    pcre2_match_data *match_data; /* regex result container */
    bool regex_error_printed;
} RegexHintSess;

/* Storage class which maps a regex to a set of servers. Note that this struct
 * does not manage the regex memory, which is done by the filter instance. */
struct RegexToServers
{
    string m_match; /* Regex in text form */
    pcre2_code* m_regex; /* Compiled regex */
    StringArray m_servers; /* List of target servers. */

    RegexToServers(string match, pcre2_code* regex)
        : m_match(match),
          m_regex(regex)
    {}

    int add_servers(string server_names);
};

/* Api entrypoints */
static MXS_FILTER *createInstance(const char *name, char **options, MXS_CONFIG_PARAMETER *params);
static MXS_FILTER_SESSION *newSession(MXS_FILTER *instance, MXS_SESSION *session);
static void closeSession(MXS_FILTER *instance, MXS_FILTER_SESSION *session);
static void freeSession(MXS_FILTER *instance, MXS_FILTER_SESSION *session);
static void setDownstream(MXS_FILTER *instance, MXS_FILTER_SESSION *fsession,
                          MXS_DOWNSTREAM *downstream);
static int routeQuery(MXS_FILTER *instance, MXS_FILTER_SESSION *fsession, GWBUF *queue);
static void diagnostic(MXS_FILTER *instance, MXS_FILTER_SESSION *fsession, DCB *dcb);
static uint64_t getCapabilities(MXS_FILTER* instance);
/* End entrypoints */

static bool validate_ip_address(const char *);
static REGEXHINT_SOURCE_HOST *set_source_address(const char *);
static void free_instance(RegexHintInst *);
static void generate_param_names(int pairs);
static void form_regex_server_mapping(MappingArray& mapping, int pcre_ops,
                                      MXS_CONFIG_PARAMETER* params);

/* These arrays contain the possible config parameter names. */
static StringArray param_names_match;
static StringArray param_names_server;

static const MXS_ENUM_VALUE option_values[] =
{
    {"ignorecase", PCRE2_CASELESS},
    {"case", 0},
    {"extended", PCRE2_EXTENDED}, // Ignore white space and # comments
    {NULL}
};

int RegexToServers::add_servers(string server_names)
{
    /* Parse the list, server names separated by ','. Do as in config.c :
     * configure_new_service() to stay compatible. We cannot check here
     * (at least not easily) if the server is named correctly, since the
     * filter doesn't even know its service. */
    char *pzServers;
    int found = 0;
    if ((pzServers = MXS_STRDUP_A(server_names.c_str())) == NULL)
    {
        return -1;
    }
    char *lasts;
    char *s = strtok_r(pzServers, ",", &lasts);
    while (s)
    {
        m_servers.push_back(s);
        found++;
        s = strtok_r(NULL, ",", &lasts);
    }
    MXS_FREE(pzServers);
    return found;
}

RegexHintInst::RegexHintInst(string user, REGEXHINT_SOURCE_HOST* source,
                             MappingArray& mapping, int ovector_size)
    :   m_user(user),
        m_source(source),
        m_mapping(mapping),
        m_ovector_size(ovector_size),
        m_total_diverted(0),
        m_total_undiverted(0)
{}

RegexHintInst::~RegexHintInst()
{
    MXS_FREE(m_source);
    for (unsigned int i = 0; i < m_mapping.size(); i++)
    {
        pcre2_code_free(m_mapping.at(i).m_regex);
    }
}

RegexHintSess_t* RegexHintInst::newSession(MXS_SESSION *session)
{
    RegexHintSess *my_session;
    const char *remote, *user;

    if ((my_session = (RegexHintSess*)MXS_CALLOC(1, sizeof(RegexHintSess))) != NULL)
    {
        my_session->n_diverted = 0;
        my_session->n_undiverted = 0;
        my_session->regex_error_printed = false;
        my_session->active = 1;
        /* It's best to generate match data from the pattern to avoid extra allocations
         * during matching. If data creation fails, matching will fail as well. */
        my_session->match_data = pcre2_match_data_create(m_ovector_size, NULL);

        /* Check client IP against 'source' host option */
        if (m_source && m_source->address &&
            (remote = session_get_remote(session)) != NULL)
        {
            my_session->active =
                this->check_source_host(remote, &session->client_dcb->ipv4);
        }

        /* Check client user against 'user' option */
        if (m_user.length() &&
            (user = session_get_user(session)) &&
            (user != m_user))
        {
            my_session->active = 0;
        }
    }
    return my_session;
}

int RegexHintInst::find_servers(RegexHintSess_t* my_session, StringArray& servers,
                                char* sql, int sql_len)
{
    /* Go through the regex array and find a match. Return the first match. */
    for (unsigned int i = 0; i < this->m_mapping.size(); i++)
    {
        pcre2_code* regex = this->m_mapping[i].m_regex;
        int result = pcre2_match(regex, (PCRE2_SPTR)sql, sql_len, 0, 0,
                                 my_session->match_data, NULL);
        if (result >= 0)
        {
            /* Have a match. No need to check if the regex matches the complete
             * query, since the user can form the regex to enforce this. */
            servers = this->m_mapping[i].m_servers;
            return result;
        }
        else if (result == PCRE2_ERROR_NOMATCH)
        {
            continue;
        }
        else
        {
            /* Error during matching */
            return result;
        }
    }
    return PCRE2_ERROR_NOMATCH;
}

int RegexHintInst::routeQuery(RegexHintSess_t* my_session, GWBUF *queue)
{
    char *sql = NULL;
    int sql_len = 0;

    if (modutil_is_SQL(queue) && my_session->active)
    {
        if (modutil_extract_SQL(queue, &sql, &sql_len))
        {
            StringArray servers;
            int result = find_servers(my_session, servers, sql, sql_len);

            if (result >= 0)
            {
                /* Add the servers in the list to the buffer routing hints */
                for (unsigned int i = 0; i < servers.size(); i++)
                {
                    queue->hint =
                        hint_create_route(queue->hint, HINT_ROUTE_TO_NAMED_SERVER,
                                          servers[i].c_str());
                }
                my_session->n_diverted++;
                m_total_diverted++;
            }
            else if (result == PCRE2_ERROR_NOMATCH)
            {
                my_session->n_undiverted++;
                m_total_undiverted++;
            }
            else
            {
                // Print regex error only once per session
                if (!my_session->regex_error_printed)
                {
                    MXS_PCRE2_PRINT_ERROR(result);
                    my_session->regex_error_printed = true;
                }
                my_session->n_undiverted++;
                m_total_undiverted++;
            }
        }
    }
    return my_session->down.routeQuery(my_session->down.instance,
                                       my_session->down.session, queue);
}

void RegexHintInst::diagnostic(RegexHintSess_t* my_session, DCB *dcb)
{
    if (this->m_mapping.size() > 0)
    {
        dcb_printf(dcb, "\t\tMatches and routes:\n");
    }
    for (unsigned int i = 0; i < this->m_mapping.size(); i++)
    {
        dcb_printf(dcb, "\t\t\t/%s/ -> ",
                   this->m_mapping[i].m_match.c_str());
        dcb_printf(dcb, "%s", this->m_mapping[i].m_servers[0].c_str());
        for (unsigned int j = 1; j < m_mapping[i].m_servers.size(); j++)
        {
            dcb_printf(dcb, ", %s", m_mapping[i].m_servers[j].c_str());
        }
        dcb_printf(dcb, "\n");
    }
    dcb_printf(dcb, "\t\tTotal no. of queries diverted by filter (approx.):     %d\n",
               m_total_diverted);
    dcb_printf(dcb, "\t\tTotal no. of queries not diverted by filter (approx.): %d\n",
               m_total_undiverted);
    if (my_session)
    {
        dcb_printf(dcb, "\t\tNo. of queries diverted by filter: %d\n",
                   my_session->n_diverted);
        dcb_printf(dcb, "\t\tNo. of queries not diverted by filter:     %d\n",
                   my_session->n_undiverted);
    }
    if (m_source)
    {
        dcb_printf(dcb,
                   "\t\tReplacement limited to connections from     %s\n",
                   m_source->address);
    }
    if (m_user.length())
    {
        dcb_printf(dcb,
                   "\t\tReplacement limit to user           %s\n",
                   m_user.c_str());
    }
}

/**
 * Check whether the client IP
 * matches the configured 'source' host
 * which can have up to three % wildcards
 *
 * @param remote      The clientIP
 * @param ipv4        The client IPv4 struct
 * @return            1 for match, 0 otherwise
 */
int RegexHintInst::check_source_host(const char *remote, const struct sockaddr_in *ipv4)
{
    int ret = 0;
    struct sockaddr_in check_ipv4;

    memcpy(&check_ipv4, ipv4, sizeof(check_ipv4));

    switch (m_source->netmask)
    {
    case 32:
        ret = strcmp(m_source->address, remote) == 0 ? 1 : 0;
        break;
    case 24:
        /* Class C check */
        check_ipv4.sin_addr.s_addr &= 0x00FFFFFF;
        break;
    case 16:
        /* Class B check */
        check_ipv4.sin_addr.s_addr &= 0x0000FFFF;
        break;
    case 8:
        /* Class A check */
        check_ipv4.sin_addr.s_addr &= 0x000000FF;
        break;
    default:
        break;
    }

    ret = (m_source->netmask < 32) ?
          (check_ipv4.sin_addr.s_addr == m_source->ipv4.sin_addr.s_addr) :
          ret;

    if (ret)
    {
        MXS_INFO("Client IP %s matches host source %s%s",
                 remote,
                 m_source->netmask < 32 ? "with wildcards " : "",
                 m_source->address);
    }

    return ret;
}

/**
 * Create an instance of the filter for a particular service
 * within MaxScale.
 *
 * @param params    The array of name/value pair parameters for the filter
 * @param options   The options for this filter
 * @param params    The array of name/value pair parameters for the filter
 *
 * @return The instance data for this new instance
 */
static MXS_FILTER*
createInstance(const char *name, char **options, MXS_CONFIG_PARAMETER *params)
{
    bool error = false;
    REGEXHINT_SOURCE_HOST* source = NULL;
    /* The cfg_param cannot be changed to string because set_source_address doesn't
       copy the contents. This inefficient as the config string searching */
    const char *cfg_param = config_get_string(params, "source");
    if (*cfg_param)
    {
        source = set_source_address(cfg_param);
        if (!source)
        {
            MXS_ERROR("Failure setting 'source' from %s", cfg_param);
            error = true;
        }
    }

    int pcre_ops = config_get_enum(params, "options", option_values);
    MappingArray mapping;
    form_regex_server_mapping(mapping, pcre_ops, params);

    if (!mapping.size() || error)
    {
        MXS_FREE(source);
        return NULL;
    }
    else
    {
        RegexHintInst* instance = NULL;
        string user(config_get_string(params, "user"));
        int ovec_size = config_get_integer(params, "ovector_size");

        MXS_EXCEPTION_GUARD(instance =
                                new RegexHintInst(user, source, mapping, ovec_size));
        return instance;
    }
}

/**
 * Associate a new session with this instance of the filter.
 *
 * @param instance  The filter instance data
 * @param session   The session itself
 * @return Session specific data for this session
 */
static MXS_FILTER_SESSION *
newSession(MXS_FILTER *instance, MXS_SESSION *session)
{
    RegexHintInst* my_instance = static_cast<RegexHintInst*>(instance);
    RegexHintSess* my_session = NULL;
    MXS_EXCEPTION_GUARD(my_session = my_instance->newSession(session));
    return (MXS_FILTER_SESSION*)my_session;
}

/**
 * Close a session with the filter, this is the mechanism
 * by which a filter may cleanup data structure etc.
 *
 * @param instance  The filter instance data
 * @param session   The session being closed
 */
static void
closeSession(MXS_FILTER *instance, MXS_FILTER_SESSION *session)
{
}

/**
 * Free the memory associated with this filter session.
 *
 * @param instance  The filter instance data
 * @param session   The session being closed
 */
static void
freeSession(MXS_FILTER *instance, MXS_FILTER_SESSION *session)
{
    RegexHintSess_t* my_session = (RegexHintSess_t*)session;
    pcre2_match_data_free(my_session->match_data);
    MXS_FREE(my_session);
    return;
}

/**
 * Set the downstream component for this filter.
 *
 * @param instance  The filter instance data
 * @param session   The session being closed
 * @param downstream    The downstream filter or router
 */
static void
setDownstream(MXS_FILTER *instance, MXS_FILTER_SESSION *session, MXS_DOWNSTREAM *downstream)
{
    RegexHintSess *my_session = (RegexHintSess *) session;
    my_session->down = *downstream;
}

/**
 * The routeQuery entry point. This is passed the query buffer
 * to which the filter should be applied. Once applied the
 * query should normally be passed to the downstream component
 * (filter or router) in the filter chain.
 *
 * If the regular expressed configured in the match parameter of the
 * filter definition matches the SQL text then add the hint
 * "Route to named server" with the name defined in the server parameter
 *
 * @param instance  The filter instance data
 * @param session   The filter session
 * @param queue     The query data
 */
static int
routeQuery(MXS_FILTER *instance, MXS_FILTER_SESSION *session, GWBUF *queue)
{
    RegexHintInst *my_instance = static_cast<RegexHintInst*>(instance);
    RegexHintSess *my_session = (RegexHintSess *) session;
    int rval = 0;
    MXS_EXCEPTION_GUARD(rval = my_instance->routeQuery(my_session, queue));
    return rval;
}

/**
 * Diagnostics routine
 *
 * If fsession is NULL then print diagnostics on the filter
 * instance as a whole, otherwise print diagnostics for the
 * particular session.
 *
 * @param   instance    The filter instance
 * @param   fsession    Filter session, may be NULL
 * @param   dcb     The DCB for diagnostic output
 */
static void
diagnostic(MXS_FILTER *instance, MXS_FILTER_SESSION *fsession, DCB *dcb)
{
    RegexHintInst *my_instance = static_cast<RegexHintInst*>(instance);
    RegexHintSess *my_session = (RegexHintSess *) fsession;
    my_instance->diagnostic(my_session, dcb);
}

/**
 * Capability routine.
 *
 * @return The capabilities of the filter.
 */
static uint64_t getCapabilities(MXS_FILTER* instance)
{
    return RCAP_TYPE_CONTIGUOUS_INPUT;
}

/**
 * Free allocated memory
 *
 * @param instance    The filter instance
 */
static void free_instance(MXS_FILTER* instance)
{
    RegexHintInst *my_instance = static_cast<RegexHintInst*>(instance);
    MXS_EXCEPTION_GUARD(delete my_instance);
}

/**
 * Read all regexes from the supplied configuration, compile them and form the mapping
 *
 * @param mapping An array of regex->serverList mappings for filling in. Is cleared on error.
 * @param pcre_ops options for pcre2_compile
 * @param params config parameters
 */
static void form_regex_server_mapping(MappingArray& mapping, int pcre_ops, MXS_CONFIG_PARAMETER* params)
{
    ss_dassert(param_names_match.size() == param_names_server.size());
    bool error = false;
    /* The config parameters can be in any order and may be skipping numbers.
     * Must just search for every possibility. Quite inefficient, but this is
     * only done once. */
    for (unsigned int i = 0; i < param_names_match.size(); i++)
    {
        const char* zMatch = param_names_match.at(i).c_str();
        const char* zServer = param_names_server.at(i).c_str();
        string match(config_get_string(params, zMatch));
        string servers(config_get_string(params, zServer));

        /* Check that both the regex and server config parameters are found */
        if (match.length() < 1 || servers.length() < 1)
        {
            if (match.length() > 0)
            {
                MXS_NOTICE("No server defined for regex setting '%s', skipping.", zMatch);
            }
            else if (servers.length() > 0)
            {
                MXS_NOTICE("No regex defined for server setting '%s', skipping.", zServer);
            }
            continue;
        }

        int errorcode = -1;
        PCRE2_SIZE error_offset = -1;
        pcre2_code* regex =
            pcre2_compile((PCRE2_SPTR) match.c_str(), match.length(), pcre_ops,
                          &errorcode, &error_offset, NULL);

        if (regex)
        {
            // Try to compile even further for faster matching
            if (pcre2_jit_compile(regex, PCRE2_JIT_COMPLETE) < 0)
            {
                MXS_NOTICE("PCRE2 JIT compilation of pattern '%s' failed, "
                           "falling back to normal compilation.", match.c_str());
            }
            RegexToServers regex_ser(match, regex);
            if (regex_ser.add_servers(servers))
            {
                mapping.push_back(regex_ser);
            }
            else
            {
                // The servers string didn't seem to contain any servers
                MXS_ERROR("Could not parse servers from string '%s'.", servers.c_str());
                pcre2_code_free(regex);
                error = true;
            }
        }
        else
        {
            MXS_ERROR("Invalid PCRE2 regular expression '%s' (position '%zu').",
                      match.c_str(), error_offset);
            MXS_PCRE2_PRINT_ERROR(errorcode);
            error = true;
        }
    }

    if (error)
    {
        for (unsigned int i = 0; i < mapping.size(); i++)
        {
            pcre2_code_free(mapping.at(i).m_regex);
        }
        mapping.clear();
    }
}

/**
 * Validate IP address string againt three dots
 * and last char not being a dot.
 *
 * Match any, '%' or '%.%.%.%', is not allowed
 *
 */
static bool validate_ip_address(const char *host)
{
    int n_dots = 0;

    /**
     * Match any is not allowed
     * Start with dot not allowed
     * Host len can't be greater than INET_ADDRSTRLEN
     */
    if (*host == '%' ||
        *host == '.' ||
        strlen(host) > INET_ADDRSTRLEN)
    {
        return false;
    }

    /* Check each byte */
    while (*host != '\0')
    {
        if (!isdigit(*host) && *host != '.' && *host != '%')
        {
            return false;
        }

        /* Dot found */
        if (*host == '.')
        {
            n_dots++;
        }

        host++;
    }

    /* Check IPv4 max number of dots and last char */
    if (n_dots == 3 && (*(host - 1) != '.'))
    {
        return true;
    }
    else
    {
        return false;
    }
}

/**
 * Set the 'source' option into a proper struct
 *
 * Input IP, which could have wildcards %, is checked
 * and the netmask 32/24/16/8 is added.
 *
 * In case of errors the 'address' field of
 * struct REGEXHINT_SOURCE_HOST is set to NULL
 *
 * @param input_host    The config source parameter
 * @return              The filled struct with netmask
 *
 */
static REGEXHINT_SOURCE_HOST *set_source_address(const char *input_host)
{
    int netmask = 32;
    int bytes = 0;
    struct sockaddr_in serv_addr;
    REGEXHINT_SOURCE_HOST *source_host =
        (REGEXHINT_SOURCE_HOST*)MXS_CALLOC(1, sizeof(REGEXHINT_SOURCE_HOST));

    if (!input_host || !source_host)
    {
        return NULL;
    }

    if (!validate_ip_address(input_host))
    {
        MXS_WARNING("The given 'source' parameter source=%s"
                    " is not a valid IP address: it will not be used.",
                    input_host);

        source_host->address = NULL;
        return source_host;
    }

    source_host->address = input_host;

    /* If no wildcards don't check it, set netmask to 32 and return */
    if (!strchr(input_host, '%'))
    {
        source_host->netmask = netmask;
        return source_host;
    }

    char format_host[strlen(input_host) + 1];
    char *p = (char *)input_host;
    char *out = format_host;

    while (*p && bytes <= 3)
    {
        if (*p == '.')
        {
            bytes++;
        }

        if (*p == '%')
        {
            *out = bytes == 3 ? '1' : '0';
            netmask -= 8;

            out++;
            p++;
        }
        else
        {
            *out++ = *p++;
        }
    }

    *out = '\0';
    source_host->netmask = netmask;

    /* fill IPv4 data struct */
    if (setipaddress(&source_host->ipv4.sin_addr, format_host) && strlen(format_host))
    {

        /* if netmask < 32 there are % wildcards */
        if (source_host->netmask < 32)
        {
            /* let's zero the last IP byte: a.b.c.0 we may have set above to 1*/
            source_host->ipv4.sin_addr.s_addr &= 0x00FFFFFF;
        }

        MXS_INFO("Input %s is valid with netmask %d\n",
                 source_host->address,
                 source_host->netmask);
    }
    else
    {
        MXS_WARNING("Found invalid IP address for parameter 'source=%s',"
                    " it will not be used.",
                    input_host);
        source_host->address = NULL;
    }

    return (REGEXHINT_SOURCE_HOST *)source_host;
}

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
extern "C" MXS_MODULE* MXS_CREATE_MODULE()
{
    static MXS_FILTER_OBJECT MyObject =
    {
        createInstance,
        newSession,
        closeSession,
        freeSession,
        setDownstream,
        NULL, // No Upstream requirement
        routeQuery,
        NULL, // No clientReply
        diagnostic,
        getCapabilities,
        free_instance, // No destroyInstance
    };

    static MXS_MODULE info =
    {
        MXS_MODULE_API_FILTER,
        MXS_MODULE_GA,
        MXS_FILTER_VERSION,
        "A routing hint filter that uses regular expressions to direct queries",
        "V1.1.0",
        &MyObject,
        NULL, /* Process init. */
        NULL, /* Process finish. */
        NULL, /* Thread init. */
        NULL, /* Thread finish. */
        {
            {"source", MXS_MODULE_PARAM_STRING},
            {"user", MXS_MODULE_PARAM_STRING},
            {"ovector_size", MXS_MODULE_PARAM_INT, "1"},
            {
                "options",
                MXS_MODULE_PARAM_ENUM,
                "ignorecase",
                MXS_MODULE_OPT_NONE,
                option_values
            },
            {MXS_END_MODULE_PARAMS}
        }
    };

    /* This module takes parameters of the form match, match01, match02, ... matchN
     * and server, server01, server02, ... serverN. The total number of module
     * parameters is limited, so let's limit the number of matches and servers.
     * First, loop over the already defined parameters... */
    int params_counter = 0;
    while (info.parameters[params_counter].name != MXS_END_MODULE_PARAMS)
    {
        params_counter++;
    }

    /* Calculate how many pairs can be added. 100 is max (to keep the postfix
     * number within two decimals). */
    const int max_pairs = 100;
    int match_server_pairs = ((MXS_MODULE_PARAM_MAX - params_counter) / 2);
    if (match_server_pairs > max_pairs)
    {
        match_server_pairs = max_pairs;
    }
    /* Create parameter pair names */
    generate_param_names(match_server_pairs);


    /* Now make the actual parameters for the module struct */
    MXS_MODULE_PARAM new_param = {NULL, MXS_MODULE_PARAM_STRING, NULL};
    for (unsigned int i = 0; i < param_names_match.size(); i++)
    {
        new_param.name = param_names_match.at(i).c_str();
        info.parameters[params_counter] = new_param;
        params_counter++;
        new_param.name = param_names_server.at(i).c_str();
        info.parameters[params_counter] = new_param;
        params_counter++;
    }

    info.parameters[params_counter].name = MXS_END_MODULE_PARAMS;

    return &info;
}

/* Generate N pairs of parameter names of form matchXX and serverXX
 *
 * @param pairs The number of parameter pairs to generate
 */
static void generate_param_names(int pairs)
{
    const char MATCH[] = "match";
    const char SERVER[] = "server";
    const int namelen_match = sizeof(MATCH) + 2;
    const int namelen_server = sizeof(SERVER) + 2;

    char name_match[namelen_match];
    char name_server[namelen_server];

    /* First, create the old "match" and "server" parameters for backwards
     * compatibility. */
    if (pairs > 0)
    {
        param_names_match.push_back(MATCH);
        param_names_server.push_back(SERVER);
    }
    /* Then all the rest. */
    const char FORMAT[] = "%s%02d";
    for (int counter = 1; counter < pairs; counter++)
    {
        ss_debug(int rval = ) snprintf(name_match, namelen_match, FORMAT, MATCH, counter);
        ss_dassert(rval == namelen_match - 1);
        ss_debug(rval = ) snprintf(name_server, namelen_server, FORMAT, SERVER, counter);
        ss_dassert(rval == namelen_server - 1);

        // Have both names, add them to the global vectors
        param_names_match.push_back(name_match);
        param_names_server.push_back(name_server);
    }
}
