# Copyright (C) 2012-2015 Codership Oy
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not, write to the
# Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston
# MA  02110-1301  USA.

# This is a common command line parser to be sourced by other SST scripts

set -u

WSREP_SST_OPT_BYPASS=0
WSREP_SST_OPT_BINLOG=""
WSREP_SST_OPT_CONF_SUFFIX=""
WSREP_SST_OPT_DATA=""
WSREP_SST_OPT_AUTH=${WSREP_SST_OPT_AUTH:-}
WSREP_SST_OPT_USER=${WSREP_SST_OPT_USER:-}
WSREP_SST_OPT_PSWD=${WSREP_SST_OPT_PSWD:-}
WSREP_SST_OPT_VERSION=""
WSREP_SST_OPT_DEBUG=""

WSREP_LOG_DEBUG=""

while [ $# -gt 0 ]; do
case "$1" in
    '--address')
        readonly WSREP_SST_OPT_ADDR="$2"
        #
        # Break address string into host:port/path parts
        #
        if echo $WSREP_SST_OPT_ADDR | grep -qe '^\[.*\]'
        then
            # IPv6 notation
            readonly WSREP_SST_OPT_HOST=${WSREP_SST_OPT_ADDR/\]*/\]}
            readonly WSREP_SST_OPT_HOST_UNESCAPED=$(echo $WSREP_SST_OPT_HOST | \
                 cut -d '[' -f 2 | cut -d ']' -f 1)
        else
            # "traditional" notation
            readonly WSREP_SST_OPT_HOST=${WSREP_SST_OPT_ADDR%%[:/]*}
        fi
        readonly WSREP_SST_OPT_PORT=$(echo $WSREP_SST_OPT_ADDR | \
                cut -d ']' -f 2 | cut -s -d ':' -f 2 | cut -d '/' -f 1)
        readonly WSREP_SST_OPT_PATH=${WSREP_SST_OPT_ADDR#*/}
        shift
        ;;
    '--bypass')
        WSREP_SST_OPT_BYPASS=1
        ;;
    '--datadir')
        readonly WSREP_SST_OPT_DATA="$2"
        shift
        ;;
    '--defaults-file')
        readonly WSREP_SST_OPT_CONF="$2"
        shift
        ;;
    '--defaults-group-suffix')
        WSREP_SST_OPT_CONF_SUFFIX="$2"
        shift
        ;;
    '--host')
        readonly WSREP_SST_OPT_HOST="$2"
        shift
        ;;
    '--local-port')
        readonly WSREP_SST_OPT_LPORT="$2"
        shift
        ;;
    '--parent')
        readonly WSREP_SST_OPT_PARENT="$2"
        shift
        ;;
    '--password')
        WSREP_SST_OPT_PSWD="$2"
        shift
        ;;
    '--port')
        readonly WSREP_SST_OPT_PORT="$2"
        shift
        ;;
    '--role')
        readonly WSREP_SST_OPT_ROLE="$2"
        shift
        ;;
    '--socket')
        readonly WSREP_SST_OPT_SOCKET="$2"
        shift
        ;;
    '--user')
        WSREP_SST_OPT_USER="$2"
        shift
        ;;
    '--mysqld-version')
        readonly WSREP_SST_OPT_VERSION="$2"
        shift
        ;;
    '--gtid')
        readonly WSREP_SST_OPT_GTID="$2"
        shift
        ;;
    '--binlog')
        WSREP_SST_OPT_BINLOG="$2"
        shift
        ;;
    '--debug')
        WSREP_SST_OPT_DEBUG="$2"
        shift
        ;;
    *) # must be command
       # usage
       # exit 1
       ;;
esac
shift
done
readonly WSREP_SST_OPT_BYPASS
readonly WSREP_SST_OPT_BINLOG
readonly WSREP_SST_OPT_CONF_SUFFIX

#
# user can specify xtrabackup specific settings that will be used during sst
# process like encryption, etc.....
# parse such configuration option. (group for xb settings is [sst] in my.cnf
#
# 1st param: group : name of the config file section, e.g. mysqld
# 2nd param: var : name of the variable in the section, e.g. server-id
# 3rd param: - : default value for the param
parse_cnf()
{
    local group=$1
    local var=$2
    local reval=""

    # normalize the variable name by replacing all '_' with '-'
    var=${var//_/-}

    # print the default settings for given group using my_print_default.
    # normalize the variable names specified in cnf file (user can use _ or - for example log-bin or log_bin)
    # then grep for needed variable
    # finally get the variable value (if variables has been specified multiple time use the last value only)

    # look in group+suffix
    if [[ -n $WSREP_SST_OPT_CONF_SUFFIX ]]; then
        reval=$($MY_PRINT_DEFAULTS -c $WSREP_SST_OPT_CONF "${group}${WSREP_SST_OPT_CONF_SUFFIX}" | awk -F= '{st=index($0,"="); cur=$0; if ($1 ~ /_/) { gsub(/_/,"-",$1);} if (st != 0) { print $1"="substr(cur,st+1) } else { print cur }}' | grep -- "--$var=" | cut -d= -f2- | tail -1)
    fi

    # look in group
    if [[ -z $reval ]]; then
        reval=$($MY_PRINT_DEFAULTS -c $WSREP_SST_OPT_CONF "${group}" | awk -F= '{st=index($0,"="); cur=$0; if ($1 ~ /_/) { gsub(/_/,"-",$1);} if (st != 0) { print $1"="substr(cur,st+1) } else { print cur }}' | grep -- "--$var=" | cut -d= -f2- | tail -1)
    fi

    # use default if we haven't found a value
    if [[ -z $reval ]]; then
        [[ -n $3 ]] && reval=$3
    fi
    echo $reval
}


#
# Converts an input string into a boolean "on", "off"
# Converts:
#   "on", "true", "1" -> "on" (case insensitive)
#   "off", "false", "0" -> "off" (case insensitive)
# All other values : -> default_value
#
# 1st param: input value
# 2nd param: default value
normalize_boolean()
{
    local input_value=$1
    local default_value=$2
    local return_value=$default_value

    if [[ "$input_value" == "1" ]]; then
        return_value="on"
    elif [[ "$input_value" =~ ^[Oo][Nn]$ ]]; then
        return_value="on"
    elif [[ "$input_value" =~ ^[Tt][Rr][Uu][Ee]$ ]]; then
        return_value="on"
    elif [[ "$input_value" == "0" ]]; then
        return_value="off"
    elif [[ "$input_value" =~ ^[Oo][Ff][Ff]$ ]]; then
        return_value="off"
    elif [[ "$input_value" =~ ^[Ff][Aa][Ll][Ss][Ee]$ ]]; then
        return_value="off"
    fi

    echo $return_value
}


# try to use my_print_defaults, mysql and mysqldump that come with the sources
# (for MTR suite)
SCRIPTS_DIR="$(cd $(dirname "$0"); pwd -P)"
EXTRA_DIR="$SCRIPTS_DIR/../extra"
CLIENT_DIR="$SCRIPTS_DIR/../client"

if [ -x "$CLIENT_DIR/mysql" ]; then
    MYSQL_CLIENT="$CLIENT_DIR/mysql"
else
    MYSQL_CLIENT=$(which mysql)
fi

if [ -x "$CLIENT_DIR/mysqldump" ]; then
    MYSQLDUMP="$CLIENT_DIR/mysqldump"
else
    MYSQLDUMP=$(which mysqldump)
fi

if [ -x "$SCRIPTS_DIR/my_print_defaults" ]; then
    MY_PRINT_DEFAULTS="$SCRIPTS_DIR/my_print_defaults"
elif [ -x "$EXTRA_DIR/my_print_defaults" ]; then
    MY_PRINT_DEFAULTS="$EXTRA_DIR/my_print_defaults"
else
    MY_PRINT_DEFAULTS=$(which my_print_defaults)
fi

wsrep_auth_not_set()
{
    [ -z "$WSREP_SST_OPT_AUTH" -o "$WSREP_SST_OPT_AUTH" = "(null)" ]
}

# For Bug:1200727
if wsrep_auth_not_set
then
    WSREP_SST_OPT_AUTH=$(parse_cnf sst wsrep-sst-auth "")
fi
readonly WSREP_SST_OPT_AUTH

# Splitting AUTH into potential user:password pair
if ! wsrep_auth_not_set
then
    readonly AUTH_VEC=(${WSREP_SST_OPT_AUTH//:/ })
    WSREP_SST_OPT_USER="${AUTH_VEC[0]:-}"
    WSREP_SST_OPT_PSWD="${AUTH_VEC[1]:-}"
fi
readonly WSREP_SST_OPT_USER
readonly WSREP_SST_OPT_PSWD

if [ -n "${WSREP_SST_OPT_DATA:-}" ]
then
    SST_PROGRESS_FILE="$WSREP_SST_OPT_DATA/sst_in_progress"
else
    SST_PROGRESS_FILE=""
fi


WSREP_LOG_DEBUG=$(parse_cnf sst wsrep-debug "")
if [ -z "$WSREP_LOG_DEBUG" ]; then
    WSREP_LOG_DEBUG=$(parse_cnf mysqld wsrep-debug "")
fi
WSREP_LOG_DEBUG=$(normalize_boolean "$WSREP_LOG_DEBUG" "off")
if [[ "$WSREP_LOG_DEBUG" == "off" ]]; then
    WSREP_LOG_DEBUG=""
fi


# Determine the timezone to use for error logging
LOGGING_TZ=$(parse_cnf mysqld log-timestamps "")
if [ -z "$LOGGING_TZ" ]; then
    LOGGING_TZ="UTC"
fi
# Set the options to the 'date' command based on the timezone
if [[ "$LOGGING_TZ" == "SYSTEM" ]]; then
    LOG_DATE_COMMAND="+%Y-%m-%dT%H:%M:%S.%6N%:z"
else
    LOG_DATE_COMMAND="-u +%Y-%m-%dT%H:%M:%S.%6NZ"
fi

wsrep_log()
{
    # echo everything to stderr so that it gets into common error log
    # deliberately made to look different from the rest of the log
    local readonly tst="$(date $LOG_DATE_COMMAND)"
    echo -e "\t$tst WSREP_SST: $*" >&2
}

wsrep_log_error()
{
    wsrep_log "[ERROR] $*"
}

wsrep_log_warning()
{
    wsrep_log "[WARNING] $*"
}

wsrep_log_info()
{
    wsrep_log "[INFO] $*"
}

wsrep_log_debug()
{
    if [[ -n "$WSREP_LOG_DEBUG" ]]; then
        wsrep_log "[DEBUG] $*"
    fi
}

wsrep_cleanup_progress_file()
{
    [ -n "${SST_PROGRESS_FILE:-}" ] && rm -f "$SST_PROGRESS_FILE" 2>/dev/null || true
}

wsrep_check_program()
{
    local prog=$1

    if ! which $prog >/dev/null
    then
        echo "'$prog' not found in PATH"
        return 2 # no such file or directory
    fi
}

wsrep_check_programs()
{
    local ret=0

    while [ $# -gt 0 ]
    do
        wsrep_check_program $1 || ret=$?
        shift
    done

    return $ret
}


# Returns the absolute path from a path to a file (with a filename)
#   If a relative path is given as an argument, the absolute path
#   is generated from the current path.
#
# Globals:
#   None
#
# Parameters:
#   Argument 1: path to a file
#
# Returns 0 if successful (path exists) and the absolute path is output.
# Returns non-zero otherwise
#
function get_absolute_path()
{
    local path="$1"
    local abs_path retvalue
    local filename

    filename=$(basename "${path}")
    abs_path=$(cd "$(dirname "${path}")" && pwd)
    retvalue=$?
    [[ $retvalue -ne 0 ]] && return $retvalue

    printf "%s/%s" "${abs_path}" "${filename}"
    return 0
}
