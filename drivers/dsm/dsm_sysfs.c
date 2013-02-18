/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 */
#include <dsm/dsm_core.h>

#define HECA_SYSFS_MODULE "heca"
#define HECA_SYSFS_RDMA "rdma"
#define HECA_SYSFS_CONN_FMT "cm_id-0x%p"
#define HECA_SYSFS_CONF "conf"
#define HECA_SYSFS_SVM_FMT "svm-%u"
#define HECA_SYSFS_DSM_FMT "dsm-%u"

#define ATTR_NAME(_name) attr_instance_##_name

#define INSTANCE_ATTR(_type, _name, _mode, _show, _store)  \
    static _type ATTR_NAME(_name) = {  \
            .attr   = {.name = __stringify(_name), .mode = _mode }, \
            .show   = _show,                    \
            .store  = _store,                   \
    };

/* default sysfs functions */
static void kobj_default_release(struct kobject *kobj)
{
    dsm_printk(KERN_DEBUG "Releasing kobject %p", kobj);
}

static ssize_t kobj_default_attr_show(struct kobject *kobj,
        struct attribute *attr, char *buf)
{
    struct kobj_attribute *kattr;
    ssize_t ret = -EIO;

    kattr = container_of(attr, struct kobj_attribute, attr);
    if (kattr->show)
        ret = kattr->show(kobj, kattr, buf);
    return ret;
}

static ssize_t kobj_default_attr_store(struct kobject *kobj, struct attribute
        *attr, const char *buf, size_t count)
{
    struct kobj_attribute *kattr;
    ssize_t ret = -EIO;

    kattr = container_of(attr, struct kobj_attribute, attr);
    if (kattr->store)
        ret = kattr->store(kobj, kattr, buf, count);
    return ret;
}

static struct sysfs_ops kobj_default_sysfs_ops = { 
    .show = kobj_default_attr_show, 
    .store = kobj_default_attr_store, 
};

static struct kobj_type kobj_default_type = { 
    .release = kobj_default_release,
    .sysfs_ops = &kobj_default_sysfs_ops,
};

/* svm sysfs functions */
void delete_svm_sysfs_entry(struct kobject *obj)
{
    kobject_put(obj);
    kobject_del(obj);
}

int create_svm_sysfs_entry(struct subvirtual_machine *svm)
{
    struct kobject *kobj = &svm->svm_sysfs.svm_kobject;
    int r;

    r = kobject_init_and_add(kobj, &kobj_default_type, &svm->dsm->dsm_kobject,
            HECA_SYSFS_SVM_FMT, svm->svm_id);
    return r;
}

/* dsm sysfs functions */
struct dsm_instance_attribute {
    struct attribute attr;
    ssize_t(*show)(struct dsm *, char *);
    ssize_t(*store)(struct dsm *, char *, size_t);
};

static ssize_t dsm_instance_show(struct kobject *k,
        struct attribute *a, char *buffer)
{
    struct dsm *dsm = container_of(k, struct dsm, dsm_kobject);
    struct dsm_instance_attribute *instance_attr = 
        container_of(a, struct dsm_instance_attribute, attr);

    if (instance_attr->show)
        return instance_attr->show(dsm, buffer);
    return 0;
}

static ssize_t instance_dsm_id_show(struct dsm *dsm,
        char *data)
{
    return sprintf(data, "%u\n", dsm->dsm_id);
}

static ssize_t instance_server_show(struct dsm *dsm,
        char *data)
{
    char s[20];
    struct dsm_module_state *dsm_state = get_dsm_module_state();

    BUG_ON(!dsm_state);
    BUG_ON(!dsm_state->rcm);

    sockaddr_ntoa(&dsm_state->rcm->sin, s, sizeof s);
    return sprintf(data, "%s\n", s);
}

INSTANCE_ATTR(struct dsm_instance_attribute, dsm_id, S_IRUGO,
        instance_dsm_id_show, NULL);
INSTANCE_ATTR(struct dsm_instance_attribute, server, S_IRUGO,
        instance_server_show, NULL);

static struct dsm_instance_attribute *dsm_instance_attr[] = {
    &ATTR_NAME(dsm_id),
    &ATTR_NAME(server),
    NULL
};

static struct sysfs_ops dsm_instance_ops = {
    .show = dsm_instance_show,
};

static struct kobj_type ktype_dsm_instance = { 
    .release = kobj_default_release,
    .sysfs_ops = &dsm_instance_ops,
    .default_attrs = (struct attribute **) dsm_instance_attr,
};

void delete_dsm_sysfs_entry(struct kobject *obj)
{
    kobject_put(obj);
    kobject_del(obj);
}

int create_dsm_sysfs_entry(struct dsm *dsm, struct dsm_module_state *dsm_state) {
    return kobject_init_and_add(&dsm->dsm_kobject, &ktype_dsm_instance,
            dsm_state->dsm_kobjects.domains_kobject, HECA_SYSFS_DSM_FMT,
            dsm->dsm_id);
}

/* conn sysfs functions */
struct conn_instance_attribute {
    struct attribute attr;
    ssize_t(*show)(struct conn_element *, char *);
    ssize_t(*store)(struct conn_element *, char *, size_t);
};

static ssize_t conn_instance_show(struct kobject *k,
        struct attribute *a, char *buffer)
{
    struct conn_element *conn = container_of(k, struct conn_element, kobj);
    struct conn_instance_attribute *instance_attr = 
        container_of(a, struct conn_instance_attribute, attr);

    if (instance_attr->show)
        return instance_attr->show(conn, buffer);
    return 0;
}

static ssize_t instance_conn_local_show(struct conn_element *conn, char *data)
{
    char s[20];
    sockaddr_ntoa(&conn->local, s, sizeof s);
    return sprintf(data, "%s\n", s);
}

static ssize_t instance_conn_remote_show(struct conn_element *conn, char *data)
{
    char s[20];
    sockaddr_ntoa(&conn->remote, s, sizeof s);
    return sprintf(data, "%s\n", s);
}

static ssize_t instance_conn_alive_show(struct conn_element *conn,
        char *data)
{
    return sprintf(data, "%d\n", atomic_read(&conn->alive));
}

INSTANCE_ATTR(struct conn_instance_attribute, conn_local, S_IRUGO,
        instance_conn_local_show, NULL);
INSTANCE_ATTR(struct conn_instance_attribute, conn_remote, S_IRUGO,
        instance_conn_remote_show, NULL);
INSTANCE_ATTR(struct conn_instance_attribute, conn_alive, S_IRUGO,
        instance_conn_alive_show, NULL);

static struct conn_instance_attribute *conn_instance_attr[] = {
    &ATTR_NAME(conn_local),
    &ATTR_NAME(conn_remote),
    &ATTR_NAME(conn_alive),
    NULL
};

static struct sysfs_ops conn_instance_ops = {
    .show = conn_instance_show,
};

static struct kobj_type ktype_conn_instance = { 
    .release = kobj_default_release,
    .sysfs_ops = &conn_instance_ops,
    .default_attrs = (struct attribute **) conn_instance_attr,
};

void delete_conn_sysfs_entry(struct conn_element *ele)
{
    kobject_put(&ele->kobj);
    kobject_del(&ele->kobj);
}

int create_conn_sysfs_entry(struct conn_element *ele)
{
    int rc;

    struct kobject *root_kobj = 
        get_dsm_module_state()->dsm_kobjects.rdma_kobject;

    rc = kobject_init_and_add(&ele->kobj,
            &ktype_conn_instance, root_kobj, 
            HECA_SYSFS_CONN_FMT, ele->cm_id);

    if (!rc)
        goto done;

done:
    return rc;
}

/* toplevel sysfs functions */
void dsm_sysfs_cleanup(struct dsm_module_state *dsm_state)
{
    struct dsm_kobjects *dsm_kobjects = &dsm_state->dsm_kobjects;

    kobject_put(dsm_kobjects->rdma_kobject);
    kobject_del(dsm_kobjects->rdma_kobject);
    kobject_put(dsm_kobjects->domains_kobject);
    kobject_del(dsm_kobjects->domains_kobject);
    kobject_del(dsm_kobjects->dsm_glob_kobject);
}

int dsm_sysfs_setup(struct dsm_module_state *dsm_state)
{
    struct dsm_kobjects *dsm_kobjects = &dsm_state->dsm_kobjects;

    dsm_kobjects->dsm_glob_kobject = kobject_create_and_add(HECA_SYSFS_MODULE,
            kernel_kobj);
    if (!dsm_kobjects->dsm_glob_kobject)
        goto err;
    dsm_kobjects->rdma_kobject = kobject_create_and_add(HECA_SYSFS_RDMA,
            dsm_kobjects->dsm_glob_kobject);
    if (!dsm_kobjects->rdma_kobject)
        goto err1;
    dsm_kobjects->domains_kobject = kobject_create_and_add(HECA_SYSFS_CONF,
            dsm_kobjects->dsm_glob_kobject);
    if (!dsm_kobjects->domains_kobject)
        goto err2;

    return 0;

err2:
    kobject_put(dsm_kobjects->rdma_kobject);
    kobject_del(dsm_kobjects->rdma_kobject);
err1:
    kobject_del(dsm_kobjects->dsm_glob_kobject);
err:
    return -ENOMEM;
}

