#ifndef SLAX_STATUS_HISTORY_H
#define SLAX_STATUS_HISTORY_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* Read external JSON values without coercion or GLib critical warnings. */
static inline gboolean status_value(JsonObject *object, const char *name, GType type)
{
    JsonNode *node=object ? json_object_get_member(object,name) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node)==type;
}

/* Missing, null and non-string fields do not acquire invented identities. */
static inline const char *status_text(JsonObject *object, const char *name)
{
    return status_value(object,name,G_TYPE_STRING) ? json_object_get_string_member(object,name) : NULL;
}

/* Snapshot identities remain exact and cannot collide with the synthetic current row. */
static inline gboolean identity(const char *value)
{
    if (!value || strlen(value)!=16) return FALSE;
    for (const char *p=value;*p;p++) if (!g_ascii_isdigit(*p) && (*p<'a' || *p>'f')) return FALSE;
    return TRUE;
}

/* Validate the complete external history before either process traverses or acts on it. */
static inline gboolean valid_history(JsonObject *catalog, GError **error)
{
    JsonNode *snapshots=catalog ? json_object_get_member(catalog,"snapshots") : NULL;
    JsonNode *current=catalog ? json_object_get_member(catalog,"currentParent") : NULL;
    if (!snapshots || !JSON_NODE_HOLDS_ARRAY(snapshots) || !current) goto invalid;
    const char *head=status_text(catalog,"currentParent");
    if (!JSON_NODE_HOLDS_NULL(current) && !identity(head)) goto invalid;
    JsonArray *array=json_node_get_array(snapshots);
    GHashTable *index=g_hash_table_new(g_str_hash,g_str_equal),*marks=g_hash_table_new(g_str_hash,g_str_equal);
    gboolean valid=TRUE;
    for (guint i=0;valid && i<json_array_get_length(array);i++)
    {
        JsonNode *node=json_array_get_element(array,i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) { valid=FALSE; break; }
        JsonObject *point=json_node_get_object(node);
        const char *id=status_text(point,"id"),*name=status_text(point,"name"),*parent=status_text(point,"parent");
        JsonNode *ancestor=json_object_get_member(point,"parent"),*size=json_object_get_member(point,"reclaimableBytes");
        valid=identity(id) && name && *name && g_utf8_validate(name,-1,NULL);
        if (valid) valid=ancestor && (JSON_NODE_HOLDS_NULL(ancestor) || identity(parent));
        if (valid) valid=status_value(point,"created",G_TYPE_INT64) && !g_hash_table_contains(index,id);
        if (valid && size && !JSON_NODE_HOLDS_NULL(size))
            valid=status_value(point,"reclaimableBytes",G_TYPE_INT64) && json_node_get_int(size)>=0;
        const char *flags[]={"deleted","automatic","protected","hidden","initial","filesOpen",NULL};
        for (guint j=0;valid && flags[j];j++)
            if (json_object_has_member(point,flags[j])) valid=status_value(point,flags[j],G_TYPE_BOOLEAN);
        if (valid) g_hash_table_insert(index,(gpointer)id,point);
    }
    if (valid && head) valid=g_hash_table_contains(index,head);
    for (guint i=0;valid && i<json_array_get_length(array);i++)
    {
        JsonObject *point=json_array_get_object_element(array,i);
        const char *parent=status_text(point,"parent");
        if (parent && !g_hash_table_contains(index,parent)) valid=FALSE;
    }
    /* Mark each ancestor chain iteratively, visiting every edge at most twice. */
    for (guint i=0;valid && i<json_array_get_length(array);i++)
    {
        const char *start=status_text(json_array_get_object_element(array,i),"id"),*id=start;
        while (id && g_hash_table_lookup(marks,id)!=GINT_TO_POINTER(2))
        {
            if (g_hash_table_contains(marks,id)) { valid=FALSE; break; }
            g_hash_table_insert(marks,(gpointer)id,GINT_TO_POINTER(1));
            id=status_text(g_hash_table_lookup(index,id),"parent");
        }
        id=start;
        while (valid && id && g_hash_table_lookup(marks,id)==GINT_TO_POINTER(1))
        {
            g_hash_table_insert(marks,(gpointer)id,GINT_TO_POINTER(2));
            id=status_text(g_hash_table_lookup(index,id),"parent");
        }
    }
    g_hash_table_unref(marks);g_hash_table_unref(index);
    if (valid) return TRUE;
invalid:
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"The saved-state history is incomplete or inconsistent.");
    return FALSE;
}

#endif
