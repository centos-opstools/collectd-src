/**
 * collectd - src/connectivity.c
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Red Hat NFVPE
 *     Andrew Bays <abays at redhat.com>
 *     Aneesh Puttur <aputtur at redhat.com>
 **/

#include "collectd.h"

#include "common.h"
#include "plugin.h"
#include "utils_complain.h"
#include "utils_ignorelist.h"

#include <asm/types.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libmnl/libmnl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <yajl/yajl_common.h>
#include <yajl/yajl_gen.h>
#if HAVE_YAJL_YAJL_VERSION_H
#include <yajl/yajl_version.h>
#endif
#if defined(YAJL_MAJOR) && (YAJL_MAJOR > 1)
#define HAVE_YAJL_V2 1
#endif

#define MYPROTO NETLINK_ROUTE

#define LINK_STATE_DOWN 0
#define LINK_STATE_UP 1
#define LINK_STATE_UNKNOWN 2

#define CONNECTIVITY_DOMAIN_FIELD "domain"
#define CONNECTIVITY_DOMAIN_VALUE "stateChange"
#define CONNECTIVITY_EVENT_ID_FIELD "eventId"
#define CONNECTIVITY_EVENT_NAME_FIELD "eventName"
#define CONNECTIVITY_EVENT_NAME_DOWN_VALUE "down"
#define CONNECTIVITY_EVENT_NAME_UP_VALUE "up"
#define CONNECTIVITY_LAST_EPOCH_MICROSEC_FIELD "lastEpochMicrosec"
#define CONNECTIVITY_PRIORITY_FIELD "priority"
#define CONNECTIVITY_PRIORITY_VALUE "high"
#define CONNECTIVITY_REPORTING_ENTITY_NAME_FIELD "reportingEntityName"
#define CONNECTIVITY_REPORTING_ENTITY_NAME_VALUE "collectd connectivity plugin"
#define CONNECTIVITY_SEQUENCE_FIELD "sequence"
#define CONNECTIVITY_SEQUENCE_VALUE "0"
#define CONNECTIVITY_SOURCE_NAME_FIELD "sourceName"
#define CONNECTIVITY_START_EPOCH_MICROSEC_FIELD "startEpochMicrosec"
#define CONNECTIVITY_VERSION_FIELD "version"
#define CONNECTIVITY_VERSION_VALUE "1.0"

#define CONNECTIVITY_NEW_STATE_FIELD "newState"
#define CONNECTIVITY_NEW_STATE_FIELD_DOWN_VALUE "outOfService"
#define CONNECTIVITY_NEW_STATE_FIELD_UP_VALUE "inService"
#define CONNECTIVITY_OLD_STATE_FIELD "oldState"
#define CONNECTIVITY_OLD_STATE_FIELD_DOWN_VALUE "outOfService"
#define CONNECTIVITY_OLD_STATE_FIELD_UP_VALUE "inService"
#define CONNECTIVITY_STATE_CHANGE_FIELDS_FIELD "stateChangeFields"
#define CONNECTIVITY_STATE_CHANGE_FIELDS_VERSION_FIELD                         \
  "stateChangeFieldsVersion"
#define CONNECTIVITY_STATE_CHANGE_FIELDS_VERSION_VALUE "1.0"
#define CONNECTIVITY_STATE_INTERFACE_FIELD "stateInterface"

/*
 * Private data types
 */

struct interface_list_s {
  char *interface;

  uint32_t status;
  uint32_t prev_status;
  uint32_t sent;
  long long unsigned int timestamp;

  struct interface_list_s *next;
};
typedef struct interface_list_s interface_list_t;

/*
 * Private variables
 */

static ignorelist_t *ignorelist = NULL;

static interface_list_t *interface_list_head = NULL;
static int monitor_all_interfaces = 1;

static int connectivity_netlink_thread_loop = 0;
static int connectivity_netlink_thread_error = 0;
static pthread_t connectivity_netlink_thread_id;
static int connectivity_dequeue_thread_loop = 0;
static int connectivity_dequeue_thread_error = 0;
static pthread_t connectivity_dequeue_thread_id;
static pthread_mutex_t connectivity_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t connectivity_cond = PTHREAD_COND_INITIALIZER;
// static struct mnl_socket *sock;
static int nl_sock = -1;
static int event_id = 0;

static const char *config_keys[] = {"Interface", "IgnoreSelected"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

/*
 * Prototype
 */

static void
connectivity_dispatch_notification(const char *interface, const char *type,
                                   gauge_t value, gauge_t old_value,
                                   long long unsigned int timestamp);

/*
 * Private functions
 */

static int gen_message_payload(int state, int old_state, const char *interface,
                               long long unsigned int timestamp, char **buf) {
  const unsigned char *buf2;
  yajl_gen g;
  char json_str[DATA_MAX_NAME_LEN];

#if !defined(HAVE_YAJL_V2)
  yajl_gen_config conf = {};

  conf.beautify = 0;
#endif

#if HAVE_YAJL_V2
  size_t len;
  g = yajl_gen_alloc(NULL);
  yajl_gen_config(g, yajl_gen_beautify, 0);
#else
  unsigned int len;
  g = yajl_gen_alloc(&conf, NULL);
#endif

  yajl_gen_clear(g);

  // *** BEGIN common event header ***

  if (yajl_gen_map_open(g) != yajl_gen_status_ok)
    goto err;

  // domain
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_DOMAIN_FIELD,
                      strlen(CONNECTIVITY_DOMAIN_FIELD)) != yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_DOMAIN_VALUE,
                      strlen(CONNECTIVITY_DOMAIN_VALUE)) != yajl_gen_status_ok)
    goto err;

  // eventId
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_EVENT_ID_FIELD,
                      strlen(CONNECTIVITY_EVENT_ID_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  event_id = event_id + 1;
  int event_id_len = sizeof(char) * sizeof(int) * 4 + 1;
  memset(json_str, '\0', DATA_MAX_NAME_LEN);
  snprintf(json_str, event_id_len, "%d", event_id);

  if (yajl_gen_number(g, json_str, strlen(json_str)) != yajl_gen_status_ok) {
    goto err;
  }

  // eventName
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_EVENT_NAME_FIELD,
                      strlen(CONNECTIVITY_EVENT_NAME_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  int event_name_len = 0;
  event_name_len = event_name_len + strlen(interface);    // interface name
  event_name_len = event_name_len + (state == 0 ? 4 : 2); // "down" or "up"
  event_name_len =
      event_name_len + 12; // "interface", 2 spaces and null-terminator
  memset(json_str, '\0', DATA_MAX_NAME_LEN);
  snprintf(json_str, event_name_len, "interface %s %s", interface,
           (state == 0 ? CONNECTIVITY_EVENT_NAME_DOWN_VALUE
                       : CONNECTIVITY_EVENT_NAME_UP_VALUE));

  if (yajl_gen_string(g, (u_char *)json_str, strlen(json_str)) !=
      yajl_gen_status_ok) {
    goto err;
  }

  // lastEpochMicrosec
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_LAST_EPOCH_MICROSEC_FIELD,
                      strlen(CONNECTIVITY_LAST_EPOCH_MICROSEC_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  int last_epoch_microsec_len =
      sizeof(char) * sizeof(long long unsigned int) * 4 + 1;
  memset(json_str, '\0', DATA_MAX_NAME_LEN);
  snprintf(json_str, last_epoch_microsec_len, "%llu",
           (long long unsigned int)CDTIME_T_TO_US(cdtime()));

  if (yajl_gen_number(g, json_str, strlen(json_str)) != yajl_gen_status_ok) {
    goto err;
  }

  // priority
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_PRIORITY_FIELD,
                      strlen(CONNECTIVITY_PRIORITY_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_PRIORITY_VALUE,
                      strlen(CONNECTIVITY_PRIORITY_VALUE)) !=
      yajl_gen_status_ok)
    goto err;

  // reportingEntityName
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_REPORTING_ENTITY_NAME_FIELD,
                      strlen(CONNECTIVITY_REPORTING_ENTITY_NAME_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_REPORTING_ENTITY_NAME_VALUE,
                      strlen(CONNECTIVITY_REPORTING_ENTITY_NAME_VALUE)) !=
      yajl_gen_status_ok)
    goto err;

  // sequence
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_SEQUENCE_FIELD,
                      strlen(CONNECTIVITY_SEQUENCE_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_number(g, CONNECTIVITY_SEQUENCE_VALUE,
                      strlen(CONNECTIVITY_SEQUENCE_VALUE)) !=
      yajl_gen_status_ok)
    goto err;

  // sourceName
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_SOURCE_NAME_FIELD,
                      strlen(CONNECTIVITY_SOURCE_NAME_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)interface, strlen(interface)) !=
      yajl_gen_status_ok)
    goto err;

  // startEpochMicrosec
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_START_EPOCH_MICROSEC_FIELD,
                      strlen(CONNECTIVITY_START_EPOCH_MICROSEC_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  int start_epoch_microsec_len =
      sizeof(char) * sizeof(long long unsigned int) * 4 + 1;
  memset(json_str, '\0', DATA_MAX_NAME_LEN);
  snprintf(json_str, start_epoch_microsec_len, "%llu",
           (long long unsigned int)timestamp);

  if (yajl_gen_number(g, json_str, strlen(json_str)) != yajl_gen_status_ok) {
    goto err;
  }

  // version
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_VERSION_FIELD,
                      strlen(CONNECTIVITY_VERSION_FIELD)) != yajl_gen_status_ok)
    goto err;

  if (yajl_gen_number(g, CONNECTIVITY_VERSION_VALUE,
                      strlen(CONNECTIVITY_VERSION_VALUE)) != yajl_gen_status_ok)
    goto err;

  // *** END common event header ***

  // *** BEGIN state change fields ***

  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_STATE_CHANGE_FIELDS_FIELD,
                      strlen(CONNECTIVITY_STATE_CHANGE_FIELDS_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_map_open(g) != yajl_gen_status_ok)
    goto err;

  // newState
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_NEW_STATE_FIELD,
                      strlen(CONNECTIVITY_NEW_STATE_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  int new_state_len =
      (state == 0 ? strlen(CONNECTIVITY_NEW_STATE_FIELD_DOWN_VALUE)
                  : strlen(CONNECTIVITY_NEW_STATE_FIELD_UP_VALUE));

  if (yajl_gen_string(
          g, (u_char *)(state == 0 ? CONNECTIVITY_NEW_STATE_FIELD_DOWN_VALUE
                                   : CONNECTIVITY_NEW_STATE_FIELD_UP_VALUE),
          new_state_len) != yajl_gen_status_ok)
    goto err;

  // oldState
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_OLD_STATE_FIELD,
                      strlen(CONNECTIVITY_OLD_STATE_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  int old_state_len =
      (old_state == 0 ? strlen(CONNECTIVITY_OLD_STATE_FIELD_DOWN_VALUE)
                      : strlen(CONNECTIVITY_OLD_STATE_FIELD_UP_VALUE));

  if (yajl_gen_string(
          g, (u_char *)(old_state == 0 ? CONNECTIVITY_OLD_STATE_FIELD_DOWN_VALUE
                                       : CONNECTIVITY_OLD_STATE_FIELD_UP_VALUE),
          old_state_len) != yajl_gen_status_ok)
    goto err;

  // stateChangeFieldsVersion
  if (yajl_gen_string(g,
                      (u_char *)CONNECTIVITY_STATE_CHANGE_FIELDS_VERSION_FIELD,
                      strlen(CONNECTIVITY_STATE_CHANGE_FIELDS_VERSION_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_number(g, CONNECTIVITY_STATE_CHANGE_FIELDS_VERSION_VALUE,
                      strlen(CONNECTIVITY_STATE_CHANGE_FIELDS_VERSION_VALUE)) !=
      yajl_gen_status_ok)
    goto err;

  // stateInterface
  if (yajl_gen_string(g, (u_char *)CONNECTIVITY_STATE_INTERFACE_FIELD,
                      strlen(CONNECTIVITY_STATE_INTERFACE_FIELD)) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)interface, strlen(interface)) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_map_close(g) != yajl_gen_status_ok)
    goto err;

  // *** END state change fields ***

  if (yajl_gen_map_close(g) != yajl_gen_status_ok)
    goto err;

  if (yajl_gen_get_buf(g, &buf2, &len) != yajl_gen_status_ok)
    goto err;

  *buf = malloc(strlen((char *)buf2) + 1);

  if (*buf == NULL) {
    char errbuf[1024];
    ERROR("connectivity plugin: malloc failed during gen_message_payload: %s",
          sstrerror(errno, errbuf, sizeof(errbuf)));
    goto err;
  }

  sstrncpy(*buf, (char *)buf2, strlen((char *)buf2) + 1);

  yajl_gen_free(g);

  return 0;

err:
  yajl_gen_free(g);
  ERROR("connectivity plugin: gen_message_payload failed to generate JSON");
  return -1;
}

static interface_list_t *add_interface(const char *interface, int status,
                                       int prev_status) {
  interface_list_t *il;
  char *interface2;

  il = malloc(sizeof(*il));
  if (il == NULL) {
    char errbuf[1024];
    ERROR("connectivity plugin: malloc failed during add_interface: %s",
          sstrerror(errno, errbuf, sizeof(errbuf)));
    return NULL;
  }

  interface2 = strdup(interface);
  if (interface2 == NULL) {
    char errbuf[1024];
    sfree(il);
    ERROR("connectivity plugin: strdup failed during add_interface: %s",
          sstrerror(errno, errbuf, sizeof(errbuf)));
    return NULL;
  }

  il->interface = interface2;
  il->status = status;
  il->prev_status = prev_status;
  il->timestamp = (long long unsigned int)CDTIME_T_TO_US(cdtime());
  il->sent = 0;
  il->next = interface_list_head;
  interface_list_head = il;

  DEBUG("connectivity plugin: added interface %s", interface2);

  return il;
}

static int connectivity_link_state(struct nlmsghdr *msg) {
  int retval = 0;
  struct ifinfomsg *ifi = mnl_nlmsg_get_payload(msg);
  struct nlattr *attr;
  const char *dev = NULL;

  pthread_mutex_lock(&connectivity_lock);

  interface_list_t *il = NULL;

  /* Scan attribute list for device name. */
  mnl_attr_for_each(attr, msg, sizeof(*ifi)) {
    if (mnl_attr_get_type(attr) != IFLA_IFNAME)
      continue;

    if (mnl_attr_validate(attr, MNL_TYPE_STRING) < 0) {
      ERROR("connectivity plugin: connectivity_link_state: IFLA_IFNAME "
            "mnl_attr_validate "
            "failed.");
      pthread_mutex_unlock(&connectivity_lock);
      return MNL_CB_ERROR;
    }

    dev = mnl_attr_get_str(attr);

    // Check the list of interfaces we should monitor, if we've chosen
    // a subset.  If we don't care about this one, abort.
    if (ignorelist_match(ignorelist, dev) != 0) {
      DEBUG("connectivity plugin: Ignoring link state change for unmonitored "
            "interface: %s",
            dev);
      break;
    }

    for (il = interface_list_head; il != NULL; il = il->next)
      if (strcmp(dev, il->interface) == 0)
        break;

    uint32_t prev_status;

    if (il == NULL) {
      // We haven't encountered this interface yet, so add it to the linked list
      il = add_interface(dev, LINK_STATE_UNKNOWN, LINK_STATE_UNKNOWN);

      if (il == NULL) {
        ERROR("connectivity plugin: unable to add interface %s during "
              "connectivity_link_state",
              dev);
        return MNL_CB_ERROR;
      }
    }

    prev_status = il->status;
    il->status =
        ((ifi->ifi_flags & IFF_RUNNING) ? LINK_STATE_UP : LINK_STATE_DOWN);
    il->timestamp = (long long unsigned int)CDTIME_T_TO_US(cdtime());

    // If the new status is different than the previous status,
    // store the previous status and set sent to zero
    if (il->status != prev_status) {
      il->prev_status = prev_status;
      il->sent = 0;
    }

    DEBUG("connectivity plugin (%llu): Interface %s status is now %s",
          il->timestamp, dev, ((ifi->ifi_flags & IFF_RUNNING) ? "UP" : "DOWN"));

    // no need to loop again, we found the interface name attr
    // (otherwise the first if-statement in the loop would
    // have moved us on with 'continue')
    break;
  }

  pthread_mutex_unlock(&connectivity_lock);

  return retval;
}

static int msg_handler(struct nlmsghdr *msg) {
  switch (msg->nlmsg_type) {
  case RTM_NEWADDR:
  case RTM_DELADDR:
  case RTM_NEWROUTE:
  case RTM_DELROUTE:
  case RTM_DELLINK:
    // Not of interest in current version
    break;
  case RTM_NEWLINK:
    connectivity_link_state(msg);
    break;
  default:
    ERROR("connectivity plugin: msg_handler: Unknown netlink nlmsg_type %d\n",
          msg->nlmsg_type);
    break;
  }
  return 0;
}

// static int read_event(struct mnl_socket *nl,
//                       int (*msg_handler)(struct nlmsghdr *)) {
static int read_event(int nl, int (*msg_handler)(struct nlmsghdr *)) {
  int status;
  int ret = 0;
  char buf[4096];
  struct nlmsghdr *h;
  int recv_flags = MSG_DONTWAIT;

  // if (nl == NULL)
  //   return ret;

  if (nl == -1)
    return ret;

  while (42) {
    pthread_mutex_lock(&connectivity_lock);

    if (connectivity_netlink_thread_loop <= 0) {
      pthread_mutex_unlock(&connectivity_lock);
      return ret;
    }

    pthread_mutex_unlock(&connectivity_lock);

    status = recv(nl, buf, sizeof(buf), recv_flags);

    if (status < 0) {

      // If there were no more messages to drain from the socket,
      // then signal the dequeue thread and allow it to dispatch
      // any saved interface status changes.  Then continue, but
      // block and wait for new messages
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        pthread_mutex_lock(&connectivity_lock);
        pthread_cond_signal(&connectivity_cond);
        pthread_mutex_unlock(&connectivity_lock);

        recv_flags = 0;
        continue;
      }

      /* Anything else is an error */
      // ERROR("connectivity plugin: read_event: Error mnl_socket_recvfrom:
      // %d\n",
      //       status);
      ERROR("connectivity plugin: read_event: Error recv: %d\n", status);
      return status;
    }

    // Message received successfully, so we'll stop blocking on the
    // receive call for now (until we get a "would block" error, which
    // will be handled above)
    recv_flags = MSG_DONTWAIT;

    if (status == 0) {
      DEBUG("connectivity plugin: read_event: EOF\n");
    }

    /* We need to handle more than one message per 'recvmsg' */
    for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned int)status);
         h = NLMSG_NEXT(h, status)) {
      /* Finish reading */
      if (h->nlmsg_type == NLMSG_DONE)
        return ret;

      /* Message is some kind of error */
      if (h->nlmsg_type == NLMSG_ERROR) {
        ERROR("connectivity plugin: read_event: Message is an error\n");
        return -1; // Error
      }

      /* Call message handler */
      if (msg_handler) {
        ret = (*msg_handler)(h);
        if (ret < 0) {
          ERROR("connectivity plugin: read_event: Message handler error %d\n",
                ret);
          return ret;
        }
      } else {
        ERROR("connectivity plugin: read_event: Error NULL message handler\n");
        return -1;
      }
    }
  }

  return ret;
}

static void send_interface_status() {
  for (interface_list_t *il = interface_list_head; il != NULL;
       il = il->next) /* {{{ */
  {
    uint32_t status;
    uint32_t prev_status;
    uint32_t sent;

    status = il->status;
    prev_status = il->prev_status;
    sent = il->sent;

    if (status != prev_status && sent == 0) {
      connectivity_dispatch_notification(il->interface, "gauge", status,
                                         prev_status, il->timestamp);
      il->sent = 1;
    }
  } /* }}} for (il = interface_list_head; il != NULL; il = il->next) */
}

static int read_interface_status() /* {{{ */
{
  pthread_mutex_lock(&connectivity_lock);

  // This first attempt is necessary because the netlink thread
  // might have held the lock while this thread was blocked on
  // the lock acquisition just above.  And while the netlink thread
  // had the lock, it could have called pthread_cond_singal, which
  // obviously wouldn't have woken this thread, since this thread
  // was not yet waiting on the condition signal.  So we need to
  // loop through the interfaces and check if any have changed
  // status before we wait on the condition signal
  send_interface_status();

  pthread_cond_wait(&connectivity_cond, &connectivity_lock);

  send_interface_status();

  pthread_mutex_unlock(&connectivity_lock);

  return 0;
} /* }}} int *read_interface_status */

static void *connectivity_netlink_thread(void *arg) /* {{{ */
{
  pthread_mutex_lock(&connectivity_lock);

  while (connectivity_netlink_thread_loop > 0) {
    int status;

    pthread_mutex_unlock(&connectivity_lock);

    status = read_event(nl_sock, msg_handler);

    pthread_mutex_lock(&connectivity_lock);

    if (status < 0) {
      connectivity_netlink_thread_error = 1;
      break;
    }

    if (connectivity_netlink_thread_loop <= 0)
      break;
  } /* while (connectivity_netlink_thread_loop > 0) */

  pthread_mutex_unlock(&connectivity_lock);

  return ((void *)0);
} /* }}} void *connectivity_netlink_thread */

static void *connectivity_dequeue_thread(void *arg) /* {{{ */
{
  pthread_mutex_lock(&connectivity_lock);

  while (connectivity_dequeue_thread_loop > 0) {
    int status;

    pthread_mutex_unlock(&connectivity_lock);

    status = read_interface_status();

    pthread_mutex_lock(&connectivity_lock);

    if (status < 0) {
      connectivity_dequeue_thread_error = 1;
      break;
    }

    if (connectivity_dequeue_thread_loop <= 0)
      break;
  } /* while (connectivity_dequeue_thread_loop > 0) */

  pthread_mutex_unlock(&connectivity_lock);

  return ((void *)0);
} /* }}} void *connectivity_dequeue_thread */

static int nl_connect() {
  int rc;
  struct sockaddr_nl sa_nl;

  nl_sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
  if (nl_sock == -1) {
    ERROR("connectivity plugin: socket open failed: %d", errno);
    return -1;
  }

  sa_nl.nl_family = AF_NETLINK;
  sa_nl.nl_groups = RTMGRP_LINK;
  sa_nl.nl_pid = getpid();

  rc = bind(nl_sock, (struct sockaddr *)&sa_nl, sizeof(sa_nl));
  if (rc == -1) {
    ERROR("connectivity plugin: socket bind failed: %d", errno);
    close(nl_sock);
    return -1;
  }

  return 0;
}

static int start_netlink_thread(void) /* {{{ */
{
  int status;

  pthread_mutex_lock(&connectivity_lock);

  if (connectivity_netlink_thread_loop != 0) {
    pthread_mutex_unlock(&connectivity_lock);
    return (0);
  }

  connectivity_netlink_thread_loop = 1;
  connectivity_netlink_thread_error = 0;

  if (nl_sock == -1) {
    status = nl_connect();

    if (status != 0)
      return status;
  }

  status = plugin_thread_create(&connectivity_netlink_thread_id,
                                /* attr = */ NULL, connectivity_netlink_thread,
                                /* arg = */ (void *)0, "connectivity");
  if (status != 0) {
    connectivity_netlink_thread_loop = 0;
    ERROR("connectivity plugin: Starting thread failed.");
    pthread_mutex_unlock(&connectivity_lock);

    int status2 = close(nl_sock);

    if (status2 != 0) {
      ERROR("connectivity plugin: failed to close socket %d: %d (%s)", nl_sock,
            status2, strerror(errno));
    } else
      nl_sock = -1;

    return (-1);
  }

  pthread_mutex_unlock(&connectivity_lock);

  return status;
}

static int start_dequeue_thread(void) /* {{{ */
{
  int status;

  pthread_mutex_lock(&connectivity_lock);

  if (connectivity_dequeue_thread_loop != 0) {
    pthread_mutex_unlock(&connectivity_lock);
    return (0);
  }

  connectivity_dequeue_thread_loop = 1;
  connectivity_dequeue_thread_error = 0;

  status = plugin_thread_create(&connectivity_dequeue_thread_id,
                                /* attr = */ NULL, connectivity_dequeue_thread,
                                /* arg = */ (void *)0, "connectivity");
  if (status != 0) {
    connectivity_dequeue_thread_loop = 0;
    ERROR("connectivity plugin: Starting dequeue thread failed.");
    pthread_mutex_unlock(&connectivity_lock);
    return (-1);
  }

  pthread_mutex_unlock(&connectivity_lock);

  return status;
} /* }}} int start_dequeue_thread */

static int start_threads(void) /* {{{ */
{
  int status, status2;

  status = start_netlink_thread();
  status2 = start_dequeue_thread();

  if (status < 0)
    return status;
  else
    return status2;
} /* }}} int start_threads */

static int stop_netlink_thread(int shutdown) /* {{{ */
{
  int status;

  if (nl_sock != -1) {
    status = close(nl_sock);
    if (status != 0) {
      ERROR("connectivity plugin: failed to close socket %d: %d (%s)", nl_sock,
            status, strerror(errno));
      return (-1);
    } else
      nl_sock = -1;
  }

  pthread_mutex_lock(&connectivity_lock);

  if (connectivity_netlink_thread_loop == 0) {
    pthread_mutex_unlock(&connectivity_lock);
    return (-1);
  }

  connectivity_netlink_thread_loop = 0;
  pthread_cond_broadcast(&connectivity_cond);
  pthread_mutex_unlock(&connectivity_lock);

  if (shutdown == 1) {
    // Since the thread is blocking, calling pthread_join
    // doesn't actually succeed in stopping it.  It will stick around
    // until a NETLINK message is received on the socket (at which
    // it will realize that "connectivity_netlink_thread_loop" is 0 and will
    // break out of the read loop and be allowed to die).  This is
    // fine when the process isn't supposed to be exiting, but in
    // the case of a process shutdown, we don't want to have an
    // idle thread hanging around.  Calling pthread_cancel here in
    // the case of a shutdown is just assures that the thread is
    // gone and that the process has been fully terminated.

    DEBUG("connectivity plugin: Canceling netlink thread for process shutdown");

    status = pthread_cancel(connectivity_netlink_thread_id);

    if (status != 0 && status != ESRCH) {
      ERROR("connectivity plugin: Unable to cancel netlink thread: %d", status);
      status = -1;
    } else
      status = 0;
  } else {
    status = pthread_join(connectivity_netlink_thread_id, /* return = */ NULL);
    if (status != 0 && status != ESRCH) {
      ERROR("connectivity plugin: Stopping netlink thread failed.");
      status = -1;
    } else
      return 0;
  }

  pthread_mutex_lock(&connectivity_lock);
  memset(&connectivity_netlink_thread_id, 0,
         sizeof(connectivity_netlink_thread_id));
  connectivity_netlink_thread_error = 0;
  pthread_mutex_unlock(&connectivity_lock);

  DEBUG("connectivity plugin: Finished requesting stop of netlink thread");

  return status;
}

static int stop_dequeue_thread(int shutdown) /* {{{ */
{
  int status;

  pthread_mutex_lock(&connectivity_lock);

  if (connectivity_dequeue_thread_loop == 0) {
    pthread_mutex_unlock(&connectivity_lock);
    return (-1);
  }

  connectivity_dequeue_thread_loop = 0;
  pthread_cond_broadcast(&connectivity_cond);
  pthread_mutex_unlock(&connectivity_lock);

  if (shutdown == 1) {
    // Calling pthread_cancel here in
    // the case of a shutdown just assures that the thread is
    // gone and that the process has been fully terminated.

    DEBUG("connectivity plugin: Canceling dequeue thread for process shutdown");

    status = pthread_cancel(connectivity_dequeue_thread_id);

    if (status != 0 && status != ESRCH) {
      ERROR("connectivity plugin: Unable to cancel dequeue thread: %d", status);
      status = -1;
    } else
      status = 0;
  } else {
    status = pthread_join(connectivity_dequeue_thread_id, /* return = */ NULL);
    if (status != 0 && status != ESRCH) {
      ERROR("connectivity plugin: Stopping dequeue thread failed.");
      status = -1;
    } else
      status = 0;
  }

  pthread_mutex_lock(&connectivity_lock);
  memset(&connectivity_dequeue_thread_id, 0,
         sizeof(connectivity_dequeue_thread_id));
  connectivity_dequeue_thread_error = 0;
  pthread_mutex_unlock(&connectivity_lock);

  DEBUG("connectivity plugin: Finished requesting stop of dequeue thread");

  return (status);
} /* }}} int stop_dequeue_thread */

static int stop_threads(int shutdown) /* {{{ */
{
  int status, status2;

  status = stop_netlink_thread(shutdown);
  status2 = stop_dequeue_thread(shutdown);

  if (status < 0)
    return status;
  else
    return status2;
} /* }}} int stop_threads */

static int connectivity_init(void) /* {{{ */
{
  if (monitor_all_interfaces) {
    NOTICE("connectivity plugin: No interfaces have been selected, so all will "
           "be monitored");
  }

  return (start_threads());
} /* }}} int connectivity_init */

static int connectivity_config(const char *key, const char *value) /* {{{ */
{
  if (ignorelist == NULL) {
    ignorelist = ignorelist_create(/* invert = */ 1);
  }

  if (strcasecmp(key, "Interface") == 0) {
    ignorelist_add(ignorelist, value);
    monitor_all_interfaces = 0;
  } else if (strcasecmp(key, "IgnoreSelected") == 0) {
    int invert = 1;
    if (IS_TRUE(value))
      invert = 0;
    ignorelist_set_invert(ignorelist, invert);
  } else {
    return (-1);
  }

  return (0);
} /* }}} int connectivity_config */

static void
connectivity_dispatch_notification(const char *interface, const char *type,
                                   gauge_t value, gauge_t old_value,
                                   long long unsigned int timestamp) {
  char *buf = NULL;
  notification_t n = {
      NOTIF_FAILURE, cdtime(), "", "", "connectivity", "", "", "", NULL};

  if (value == LINK_STATE_UP)
    n.severity = NOTIF_OKAY;

  sstrncpy(n.host, hostname_g, sizeof(n.host));
  sstrncpy(n.plugin_instance, interface, sizeof(n.plugin_instance));
  sstrncpy(n.type, "gauge", sizeof(n.type));
  sstrncpy(n.type_instance, "interface_status", sizeof(n.type_instance));

  gen_message_payload(value, old_value, interface, timestamp, &buf);

  notification_meta_t *m = calloc(1, sizeof(*m));

  if (m == NULL) {
    char errbuf[1024];
    sfree(buf);
    ERROR("connectivity plugin: unable to allocate metadata: %s",
          sstrerror(errno, errbuf, sizeof(errbuf)));
    return;
  }

  sstrncpy(m->name, "ves", sizeof(m->name));
  m->nm_value.nm_string = sstrdup(buf);
  m->type = NM_TYPE_STRING;
  n.meta = m;

  DEBUG("connectivity plugin: notification message: %s",
        n.meta->nm_value.nm_string);

  DEBUG("connectivity plugin: dispatching state %d for interface %s",
        (int)value, interface);

  plugin_dispatch_notification(&n);
  plugin_notification_meta_free(n.meta);

  // malloc'd in gen_message_payload
  if (buf != NULL)
    sfree(buf);
}

static int connectivity_read(void) /* {{{ */
{
  pthread_mutex_lock(&connectivity_lock);

  if (connectivity_netlink_thread_error != 0) {

    pthread_mutex_unlock(&connectivity_lock);

    ERROR("connectivity plugin: The netlink thread had a problem. Restarting "
          "it.");

    stop_netlink_thread(0);

    for (interface_list_t *il = interface_list_head; il != NULL;
         il = il->next) {
      il->status = LINK_STATE_UNKNOWN;
      il->prev_status = LINK_STATE_UNKNOWN;
      il->sent = 0;
    }

    start_netlink_thread();

    return (-1);
  } /* if (connectivity_netlink_thread_error != 0) */

  if (connectivity_dequeue_thread_error != 0) {

    pthread_mutex_unlock(&connectivity_lock);

    ERROR("connectivity plugin: The dequeue thread had a problem. Restarting "
          "it.");

    stop_dequeue_thread(0);

    start_dequeue_thread();

    return (-1);
  } /* if (connectivity_dequeue_thread_error != 0) */

  pthread_mutex_unlock(&connectivity_lock);

  return (0);
} /* }}} int connectivity_read */

static int connectivity_shutdown(void) /* {{{ */
{
  interface_list_t *il;

  DEBUG("connectivity plugin: Shutting down thread.");
  if (stop_threads(1) < 0)
    return (-1);

  il = interface_list_head;
  while (il != NULL) {
    interface_list_t *il_next;

    il_next = il->next;

    sfree(il->interface);
    sfree(il);

    il = il_next;
  }

  ignorelist_free(ignorelist);

  return (0);
} /* }}} int connectivity_shutdown */

void module_register(void) {
  plugin_register_config("connectivity", connectivity_config, config_keys,
                         config_keys_num);
  plugin_register_init("connectivity", connectivity_init);
  plugin_register_read("connectivity", connectivity_read);
  plugin_register_shutdown("connectivity", connectivity_shutdown);
} /* void module_register */
