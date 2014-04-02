#include <stdio.h>
#include <stdlib.h>

#include "vrt.h"
#include "cache/cache.h"

#include "vtree.h"
#include <sys/time.h>

#include "vcc_if.h"

static double VTIM_real(void);

struct tkey {
	const char *key;
	VRB_ENTRY(tkey) tree;
};

static int
keycmp(const struct tkey *k1, const struct tkey *k2) {
	return (strcmp(k1->key, k2->key));
}

VRB_HEAD(tbtree, tkey);
VRB_PROTOTYPE_STATIC(tbtree, tkey, tree, keycmp);
VRB_GENERATE_STATIC(tbtree, tkey, tree, keycmp);

static unsigned n_init;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static struct tbtree tbs = VRB_INITIALIZER(&tbs);

/* Represents a token bucket for a specific key. */
struct tbucket {
	struct tkey key;
	unsigned magic;
#define TBUCKET_MAGIC 0x53345eb9
	double last_used;
	double period;
	long tokens;
	long capacity;

	/* TODO: be clever about locking/mutex granularity. */
};

struct tbucket *
tb_alloc(const char *key, long limit, double period) {
	struct tbucket *tb = malloc(sizeof *tb);
	AN(tb);

	tb->key.key = strdup(key);
	tb->magic = TBUCKET_MAGIC;
	tb->last_used = VTIM_real();
	tb->period = period;
	tb->tokens = limit;
	tb->capacity = limit;

	return (tb);
}

static struct tbucket *
get_bucket(const char *key, long limit, double period) {
	struct tbucket *b;
	struct tkey *pkey;
	struct tkey k = { .key = key };

	pkey = VRB_FIND(tbtree, &tbs, &k);
	if (pkey) {
		CAST_OBJ_NOTNULL(b, (void *) pkey, TBUCKET_MAGIC);
	} else {
		b = tb_alloc(key, limit, period);
		AZ(VRB_INSERT(tbtree, &tbs, &b->key));
	}
	return (b);
}

/* Borrow this from vtim.c */
static double
VTIM_real(void) {
	struct timeval tv;
	assert(gettimeofday(&tv, NULL) == 0);
	return (tv.tv_sec + 1e-6 * tv.tv_usec);
}

static void
add_tokens(struct tbucket *b, double now) {
	double delta = now - b->last_used;

	b->tokens += (long) ((delta / b->period) * b->capacity);
	if (b->tokens > b->capacity)
		b->tokens = b->capacity;
	/* VSL(SLT_VCL_Log, 0, "tokens: %ld", b->tokens); */
}

VCL_BOOL
vmod_is_denied(const struct vrt_ctx *ctx, VCL_STRING key, VCL_INT limit,
    VCL_DURATION period) {
	unsigned ret = 1;
	struct tbucket *b;
	double now = VTIM_real();

	AZ(pthread_mutex_lock(&mtx));

	b = get_bucket(key, limit, period);
	add_tokens(b, now);
	if (b->tokens > 0) {
		b->tokens -= 1;
		ret = 0;
		b->last_used = now;
	}

	AZ(pthread_mutex_unlock(&mtx));
	return (ret);
}


static void
fini(void *priv)
{
	assert(priv == &n_init);

	AZ(pthread_mutex_lock(&mtx));
	assert(n_init > 0);
	n_init--;
	if (n_init == 0) {
		/* Do cleanup */
	}
	AZ(pthread_mutex_unlock(&mtx));
}

int
init(struct vmod_priv *priv, const struct VCL_conf *conf)
{
	priv->priv = &n_init;
	priv->free = fini;
	AZ(pthread_mutex_lock(&mtx));
	if (n_init == 0) {
		/* Do initial setup.  */
	}
	n_init++;
	AZ(pthread_mutex_unlock(&mtx));
	return (0);
}
