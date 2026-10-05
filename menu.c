#define _GNU_SOURCE
#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct MangobarMenu {
  sd_bus *bus;
  char *service;
  char *path;
  MangobarMenuNode *root; // full tree from GetLayout
  MangobarMenuNode *current; // node currently shown (may be a submenu)
  void (*on_layout)(void *data);
  void *userdata;
  sd_bus_slot *layout_slot;
  bool has_layout;
};

static void free_node(MangobarMenuNode *n) {
  if (!n)
    return;
  free(n->label);
  free(n->type);
  free(n->toggle_type);
  for (int i = 0; i < n->child_count; i++)
    free_node(n->children[i]);
  free(n->children);
  free(n);
}

MangobarMenu *menu_init(sd_bus *bus, const char *service, const char *path,
                        void (*on_layout)(void *data), void *userdata) {
  MangobarMenu *m = calloc(1, sizeof(*m));
  if (!m)
    return NULL;
  m->bus = bus;
  m->service = strdup(service);
  m->path = strdup(path);
  m->on_layout = on_layout;
  m->userdata = userdata;
  return m;
}

void menu_destroy(MangobarMenu *m) {
  if (!m)
    return;
  sd_bus_slot_unref(m->layout_slot);
  free_node(m->root);
  free(m->service);
  free(m->path);
  free(m);
}

// ---------- GetLayout parsing ----------
static int parse_node(sd_bus_message *msg, MangobarMenuNode **out) {
  MangobarMenuNode *n = calloc(1, sizeof(*n));
  if (!n)
    return -12;
  n->visible = true; // visible defaults to true
  n->enabled = true; // enabled defaults to true
  int ret = sd_bus_message_enter_container(msg, 'r', "ia{sv}av");
  if (ret < 0) {
    free(n);
    return ret;
  }
  ret = sd_bus_message_read(msg, "i", &n->id);
  if (ret < 0) {
    free(n);
    return ret;
  }

  // Properties a{sv}
  ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
  if (ret < 0)
    goto err;
  while (!sd_bus_message_at_end(msg, 0)) {
    if (sd_bus_message_enter_container(msg, 'e', "sv") < 0)
      break;
    char *key = NULL;
    if (sd_bus_message_read(msg, "s", &key) < 0) {
      sd_bus_message_exit_container(msg);
      break;
    }
    if (sd_bus_message_enter_container(msg, 'v', NULL) < 0) {
      sd_bus_message_exit_container(msg);
      break;
    }
    if (strcmp(key, "label") == 0) {
      char *s = NULL;
      if (sd_bus_message_read(msg, "s", &s) >= 0 && s) {
        free(n->label);
        n->label = strdup(s);
      }
    } else if (strcmp(key, "type") == 0) {
      char *s = NULL;
      if (sd_bus_message_read(msg, "s", &s) >= 0 && s) {
        free(n->type);
        n->type = strdup(s);
      }
    } else if (strcmp(key, "enabled") == 0) {
      int b = 0;
      if (sd_bus_message_read(msg, "b", &b) >= 0)
        n->enabled = b;
    } else if (strcmp(key, "visible") == 0) {
      int b = 0;
      if (sd_bus_message_read(msg, "b", &b) >= 0)
        n->visible = b;
    } else if (strcmp(key, "toggle-state") == 0) {
      int v = 0;
      if (sd_bus_message_read(msg, "i", &v) >= 0)
        n->toggle_state = v;
    } else if (strcmp(key, "toggle-type") == 0) {
      char *s = NULL;
      if (sd_bus_message_read(msg, "s", &s) >= 0 && s) {
        free(n->toggle_type);
        n->toggle_type = strdup(s);
      }
    } else {
      // Peek type then skip, keeping the container state consistent
      char t;
      const char *sig;
      sd_bus_message_peek_type(msg, &t, &sig);
      if (t == 's' || t == 'o') {
        char *s = NULL;
        sd_bus_message_read(msg, "s", &s);
      } else if (t == 'b') {
        int b = 0;
        sd_bus_message_read(msg, "b", &b);
      } else {
        sd_bus_message_skip(msg, NULL);
      }
    }
    sd_bus_message_exit_container(msg); // v
    sd_bus_message_exit_container(msg); // e
  }
  sd_bus_message_exit_container(msg); // a

  // Children av
  ret = sd_bus_message_enter_container(msg, 'a', "v");
  if (ret < 0)
    goto err;
  while (!sd_bus_message_at_end(msg, 0)) {
    if (sd_bus_message_enter_container(msg, 'v', "(ia{sv}av)") < 0)
      break;
    MangobarMenuNode *child = NULL;
    parse_node(msg, &child);
    if (child) {
      n->children =
          realloc(n->children, sizeof(*n->children) * (n->child_count + 1));
      n->children[n->child_count++] = child;
    }
    sd_bus_message_exit_container(msg); // v
  }
  sd_bus_message_exit_container(msg); // a
  sd_bus_message_exit_container(msg); // r
  *out = n;
  return 0;
err:
  free_node(n);
  return ret;
}

static int layout_callback(sd_bus_message *msg, void *data,
                           sd_bus_error *error) {
  MangobarMenu *m = data;
  m->layout_slot = sd_bus_slot_unref(m->layout_slot);
  if (sd_bus_message_is_method_error(msg, NULL)) {
    m->has_layout = false;
    if (m->on_layout)
      m->on_layout(m->userdata);
    return 0;
  }

  // GetLayout returns: u revision + (ia{sv}av) root node
  uint32_t revision = 0;
  sd_bus_message_read(msg, "u", &revision);
  MangobarMenuNode *root = NULL;
  int ret = parse_node(msg, &root);
  if (ret < 0 || !root)
    return 0;

  free_node(m->root);
  m->root = root;
  m->current = root;
  m->has_layout = true;
  if (m->on_layout)
    m->on_layout(m->userdata);
  return 0;
}

void menu_refresh(MangobarMenu *m) {
  if (!m || m->layout_slot)
    return;
  // AboutToShow(0) tells the service to prepare the menu
  sd_bus_call_method_async(m->bus, NULL, m->service, m->path,
                           "com.canonical.dbusmenu", "AboutToShow", NULL, NULL,
                           "i", 0);
  // GetLayout fetches the layout
  sd_bus_call_method_async(m->bus, &m->layout_slot, m->service, m->path,
                           "com.canonical.dbusmenu", "GetLayout",
                           layout_callback, m, "iias", 0, -1, NULL);
}

bool menu_has_items(MangobarMenu *m) {
  return m && m->has_layout && m->current &&
         menu_visible_count(m) > 0;
}

int menu_visible_count(MangobarMenu *m) {
  if (!m || !m->current)
    return 0;
  int count = 0;
  for (int i = 0; i < m->current->child_count; i++) {
    const MangobarMenuNode *n = m->current->children[i];
    if (n->visible && n->label && *n->label)
      count++;
  }
  return count;
}

const MangobarMenuNode *menu_visible_node(MangobarMenu *m, int idx) {
  if (!m || !m->current || idx < 0)
    return NULL;
  int found = -1;
  for (int i = 0; i < m->current->child_count; i++) {
    const MangobarMenuNode *n = m->current->children[i];
    if (n->visible && n->label && *n->label) {
      found++;
      if (found == idx)
        return n;
    }
  }
  return NULL;
}

int menu_node_visible_count(const MangobarMenuNode *node) {
  if (!node)
    return 0;
  int count = 0;
  for (int i = 0; i < node->child_count; i++) {
    const MangobarMenuNode *n = node->children[i];
    if (n->visible && n->label && *n->label)
      count++;
  }
  return count;
}

const MangobarMenuNode *menu_node_visible_node(const MangobarMenuNode *node,
                                               int idx) {
  if (!node || idx < 0)
    return NULL;
  int found = -1;
  for (int i = 0; i < node->child_count; i++) {
    const MangobarMenuNode *n = node->children[i];
    if (n->visible && n->label && *n->label) {
      found++;
      if (found == idx)
        return n;
    }
  }
  return NULL;
}

bool menu_enter_child(MangobarMenu *m, const MangobarMenuNode *node) {
  if (!m || !node)
    return false;
  // Only items with children can be entered
  for (int i = 0; i < m->current->child_count; i++)
    if (m->current->children[i] == node && node->child_count > 0) {
      m->current = (MangobarMenuNode *)node;
      if (m->on_layout)
        m->on_layout(m->userdata);
      return true;
    }
  return false;
}

bool menu_go_back(MangobarMenu *m) {
  if (!m)
    return false;
  if (m->current == m->root)
    return false;
  m->current = m->root;
  if (m->on_layout)
    m->on_layout(m->userdata);
  return true;
}

bool menu_in_submenu(MangobarMenu *m) {
  return m && m->current != NULL && m->current != m->root;
}

void menu_activate(MangobarMenu *m, const MangobarMenuNode *node,
                   uint32_t timestamp) {
  if (!m || !node || !node->enabled)
    return;
  sd_bus_message *msg = NULL;
  if (sd_bus_message_new_method_call(m->bus, &msg, m->service, m->path,
                                     "com.canonical.dbusmenu",
                                     "Event") < 0)
    return;
  sd_bus_message_append(msg, "is", node->id, "clicked");
  // data variant: toggle -> bool, radio -> value, else empty string
  const char *toggle = node->toggle_type ? node->toggle_type : node->type;
  if (toggle && (strcmp(toggle, "checkmark") == 0 ||
                 strcmp(toggle, "toggle") == 0)) {
    sd_bus_message_open_container(msg, 'v', "b");
    sd_bus_message_append(msg, "b", node->toggle_state ? 0 : 1);
    sd_bus_message_close_container(msg);
  } else if (toggle && strcmp(toggle, "radio") == 0) {
    sd_bus_message_open_container(msg, 'v', "i");
    sd_bus_message_append(msg, "i", node->id);
    sd_bus_message_close_container(msg);
  } else {
    sd_bus_message_open_container(msg, 'v', "s");
    sd_bus_message_append(msg, "s", "");
    sd_bus_message_close_container(msg);
  }
  sd_bus_message_append(msg, "u", timestamp);
  sd_bus_send(m->bus, msg, NULL);
  sd_bus_message_unref(msg);
}
