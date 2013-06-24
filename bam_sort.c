#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ksort.h"
#include "bam_stat.h"
#include "bam_index.h"

static int g_is_by_qname = 0;
static int g_ignore_warts = 0;

static inline int strnum_cmp(const char *a, const char *b)
{
    if( g_ignore_warts ) {
        while( (a[0] == 'M' || a[0] == 'F' || a[0] == 'R' || a[0] == 'C') && a[1] == '_' ) a+=2 ;
        while( (b[0] == 'M' || b[0] == 'F' || b[0] == 'R' || b[0] == 'C') && b[1] == '_' ) b+=2 ;
    }

	char *pa, *pb;
	pa = (char*)a; pb = (char*)b;
	while (*pa && *pb) {
		if (isdigit(*pa) && isdigit(*pb)) {
			long ai, bi;
			ai = strtol(pa, &pa, 10);
			bi = strtol(pb, &pb, 10);
			if (ai != bi) return ai<bi? -1 : ai>bi? 1 : 0;
		} else {
			if (*pa != *pb) break;
			++pa; ++pb;
		}
	}
	if (*pa == *pb)
		return (pa-a) < (pb-b)? -1 : (pa-a) > (pb-b)? 1 : 0;
	return *pa<*pb? -1 : *pa>*pb? 1 : 0;
}

#define HEAP_EMPTY 0xffffffffffffffffull

typedef struct {
	int i;
	uint64_t pos, idx;
	bam1_t *b;
} heap1_t;

#define __pos_cmp(a, b) ((a).pos > (b).pos || ((a).pos == (b).pos && ((a).i > (b).i || ((a).i == (b).i && (a).idx > (b).idx))))
#define __flag_cmp(a,b) ( ((a).flags & (...)) > ((b).flags & (...)))

static inline int heap_lt(const heap1_t a, const heap1_t b)
{
	if (g_is_by_qname) {
		int t;
		if (a.b == 0 || b.b == 0) return a.b == 0? 1 : 0;
		t = strnum_cmp(bam1_qname(a.b), bam1_qname(b.b));
        return t > 0 ? 1 :
               t < 0 ? 0 :
               (a.b->core.flag & BAM_FPAIRED) < (b.b->core.flag & BAM_FPAIRED) ? 1 :
               (a.b->core.flag & BAM_FPAIRED) > (b.b->core.flag & BAM_FPAIRED) ? 0 :
               (a.b->core.flag & (BAM_FREAD1|BAM_FREAD2)) > (b.b->core.flag & (BAM_FREAD1|BAM_FREAD2)) ;
	} else return __pos_cmp(a, b);
}

KSORT_INIT(heap, heap1_t, heap_lt)

static void swap_header_targets(bam_header_t *h1, bam_header_t *h2)
{
	bam_header_t t;
	t.n_targets = h1->n_targets, h1->n_targets = h2->n_targets, h2->n_targets = t.n_targets;
	t.target_name = h1->target_name, h1->target_name = h2->target_name, h2->target_name = t.target_name;
	t.target_len = h1->target_len, h1->target_len = h2->target_len, h2->target_len = t.target_len;
}

static void swap_header_text(bam_header_t *h1, bam_header_t *h2)
{
	int tempi;
	char *temps;
	tempi = h1->l_text, h1->l_text = h2->l_text, h2->l_text = tempi;
	temps = h1->text, h1->text = h2->text, h2->text = temps;
}

#define MERGE_RG     1
#define MERGE_UNCOMP 2
#define MERGE_LEVEL1 4
#define MERGE_FORCE  8

/*!
  @abstract    Merge multiple sorted BAM.
  @param  is_by_qname whether to sort by query name
  @param  out  output BAM file name
  @param  headers  name of SAM file from which to copy '@' header lines,
                   or NULL to copy them from the first file to be merged
  @param  n    number of files to be merged
  @param  fn   names of files to be merged

  @discussion Padding information may NOT correctly maintained. This
  function is NOT thread safe.

  @discussion Calling this "core" is an euphemism.  Merging should never
  have been conflated with file I/O.
 */

struct bam_sink {
    bamFile fp;
    bam_header_t *hdr;
    struct flagstatx_acc fa;
    struct covstat_acc ca;
    struct index_acc ia;
} ;

void bam_sink_init_file( struct bam_sink *s, const char* out, int flag ) 
{
    memset( s, 0, sizeof(struct bam_sink) );
	if (flag & MERGE_UNCOMP) s->fp = strcmp(out, "-")? bam_open(out, "wu") : bam_dopen(fileno(stdout), "wu");
	else if (flag & MERGE_LEVEL1) s->fp = strcmp(out, "-")? bam_open(out, "w1") : bam_dopen(fileno(stdout), "w1");
	else s->fp = strcmp(out, "-")? bam_open(out, "w") : bam_dopen(fileno(stdout), "w");
	if (s->fp == 0) {
		fprintf(stderr, "[%s] fail to create the output file.\n", __func__);
		exit(1);
	}
}

inline void bam_put_header( struct bam_sink *s, bam_header_t *h )
{
    s->hdr = h;
    if(s->fp) {
        bam_header_write(s->fp, h);
        if(s->ia.idx)
            index_acc_init_B(&s->ia, h->n_targets, bam_tell(s->fp) );
    }
}

inline void bam_put_rec( struct bam_sink *s, bam1_t *b )
{
    const char *rg = get_rg(b);
    if(s->fp) {
        bam_write1_core(s->fp, &b->core, b->data_len, b->data);
        if(s->ia.idx)
            index_acc_step(&s->ia, b, bam_tell(s->fp)) ;
    }
    if(s->fa.h) flagstatx_step(&s->fa, rg, b);
    if(s->ca.h && s->hdr) covstat_step(&s->ca, rg, s->hdr, b);
}

inline void bam_sink_close( struct bam_sink *s ) 
{
	bam_close(s->fp);
    if(s->fa.h) flagstatx_destroy(&s->fa);
    if(s->ca.h) covstat_destroy(&s->ca);
    if(s->hdr) bam_header_destroy(s->hdr);
    if(s->ia.idx) bam_index_destroy(s->ia.idx);
}

int bam_merge_core_ext(int by_qname, struct bam_sink *fpout, const char *headers, int n, char * const *fn,
					int flag, const char *reg)
{
	bamFile *fp;
	heap1_t *heap;
	bam_header_t *hout = 0;
	bam_header_t *hheaders = NULL;
	int i, j, *RG_len = 0;
	uint64_t idx = 0;
	char **RG = 0;
	bam_iter_t *iter = 0;

	if (headers) {
		tamFile fpheaders = sam_open(headers);
		if (fpheaders == 0) {
			const char *message = strerror(errno);
			fprintf(stderr, "[%s] cannot open '%s': %s\n", __func__, headers, message);
			return -1;
		}
		hheaders = sam_header_read(fpheaders);
		sam_close(fpheaders);
	}

	g_is_by_qname = by_qname;
	fp = (bamFile*)calloc(n, sizeof(bamFile));
	heap = (heap1_t*)calloc(n, sizeof(heap1_t));
	iter = (bam_iter_t*)calloc(n, sizeof(bam_iter_t));
	// prepare RG tag
	if (flag & MERGE_RG) {
		RG = (char**)calloc(n, sizeof(void*));
		RG_len = (int*)calloc(n, sizeof(int));
		for (i = 0; i != n; ++i) {
			int l = strlen(fn[i]);
			const char *s = fn[i];
			if (l > 4 && strcmp(s + l - 4, ".bam") == 0) l -= 4;
			for (j = l - 1; j >= 0; --j) if (s[j] == '/') break;
			++j; l -= j;
			RG[i] = calloc(l + 1, 1);
			RG_len[i] = l;
			strncpy(RG[i], s + j, l);
		}
	}
	// read the first
	for (i = 0; i != n; ++i) {
		bam_header_t *hin;
		fp[i] = bam_open(fn[i], "r");
		if (fp[i] == 0) {
			int j;
			fprintf(stderr, "[%s] fail to open file %s\n", __func__, fn[i]);
			for (j = 0; j < i; ++j) bam_close(fp[j]);
			free(fp); free(heap);
			// FIXME: possible memory leak
			return -1;
		}
		hin = bam_header_read(fp[i]);
		if (i == 0) { // the first BAM
			hout = hin;
		} else { // validate multiple baf
			int min_n_targets = hout->n_targets;
			if (hin->n_targets < min_n_targets) min_n_targets = hin->n_targets;

			for (j = 0; j < min_n_targets; ++j)
				if (strcmp(hout->target_name[j], hin->target_name[j]) != 0) {
					fprintf(stderr, "[%s] different target sequence name: '%s' != '%s' in file '%s'\n",
							__func__, hout->target_name[j], hin->target_name[j], fn[i]);
					return -1;
				}

			// If this input file has additional target reference sequences,
			// add them to the headers to be output
			if (hin->n_targets > hout->n_targets) {
				swap_header_targets(hout, hin);
				// FIXME Possibly we should also create @SQ text headers
				// for the newly added reference sequences
			}

			bam_header_destroy(hin);
		}
	}

	if (hheaders) {
		// If the text headers to be swapped in include any @SQ headers,
		// check that they are consistent with the existing binary list
		// of reference information.
		if (hheaders->n_targets > 0) {
			if (hout->n_targets != hheaders->n_targets) {
				fprintf(stderr, "[%s] number of @SQ headers in '%s' differs from number of target sequences\n", __func__, headers);
				if (!reg) return -1;
			}
			for (j = 0; j < hout->n_targets; ++j)
				if (strcmp(hout->target_name[j], hheaders->target_name[j]) != 0) {
					fprintf(stderr, "[%s] @SQ header '%s' in '%s' differs from target sequence\n", __func__, hheaders->target_name[j], headers);
					if (!reg) return -1;
				}
		}

		swap_header_text(hout, hheaders);
		bam_header_destroy(hheaders);
	}

	if (reg) {
		int tid, beg, end;
		if (bam_parse_region(hout, reg, &tid, &beg, &end) < 0) {
			fprintf(stderr, "[%s] Malformated region string or undefined reference name\n", __func__);
			return -1;
		}
		for (i = 0; i < n; ++i) {
			bam_index_t *idx;
			idx = bam_index_load(fn[i]);
			iter[i] = bam_iter_query(idx, tid, beg, end);
			bam_index_destroy(idx);
		}
	}

	for (i = 0; i < n; ++i) {
		heap1_t *h = heap + i;
		h->i = i;
		h->b = (bam1_t*)calloc(1, sizeof(bam1_t));
		if (bam_iter_read(fp[i], iter[i], h->b) >= 0) {
			h->pos = ((uint64_t)h->b->core.tid<<32) | (uint32_t)((int32_t)h->b->core.pos+1)<<1 | bam1_strand(h->b);
			h->idx = idx++;
		}
		else h->pos = HEAP_EMPTY;
	}
	bam_put_header(fpout, hout);

	ks_heapmake(heap, n, heap);
	while (heap->pos != HEAP_EMPTY) {
		bam1_t *b = heap->b;
		if (flag & MERGE_RG) {
			uint8_t *rg = bam_aux_get(b, "RG");
			if (rg) bam_aux_del(b, rg);
			bam_aux_append(b, "RG", 'Z', RG_len[heap->i] + 1, (uint8_t*)RG[heap->i]);
		}
		bam_put_rec(fpout, b);
		if ((j = bam_iter_read(fp[heap->i], iter[heap->i], b)) >= 0) {
			heap->pos = ((uint64_t)b->core.tid<<32) | (uint32_t)((int)b->core.pos+1)<<1 | bam1_strand(b);
			heap->idx = idx++;
		} else if (j == -1) {
			heap->pos = HEAP_EMPTY;
			free(heap->b->data); free(heap->b);
			heap->b = 0;
		} else fprintf(stderr, "[%s] '%s' is truncated. Continue anyway.\n", __func__, fn[heap->i]);
		ks_heapadjust(heap, 0, n, heap);
	}

	if (flag & MERGE_RG) {
		for (i = 0; i != n; ++i) free(RG[i]);
		free(RG); free(RG_len);
	}
	for (i = 0; i != n; ++i) {
		bam_iter_destroy(iter[i]);
		bam_close(fp[i]);
	}
	free(fp); free(heap); free(iter);
	return 0;
}

int bam_merge_core(int by_qname, const char *out, const char *headers, int n, char * const *fn,
					int flag, const char *reg)
{
    struct bam_sink fpout;
    bam_sink_init_file(&fpout, out, flag);
    int r = bam_merge_core_ext(by_qname, &fpout, headers, n, fn, flag, reg);
    bam_sink_close(&fpout);
    return r;
}


int bam_merge(int argc, char *argv[], int vanilla)
{
	int c, is_by_qname = 0, flag = 0, ret = 0;
	char *fn_headers = NULL, *reg = 0, *oname = "-";
    char *fn_index = 0, *fn_cstat = 0, *fn_xstat = 0;

	while ((c = getopt(argc, argv, vanilla?"h:nru1R:":"h:nru1R:fo:i:x:c:")) >= 0) {
		switch (c) {
		case 'r': flag |= MERGE_RG; break;
		case 'f': flag |= MERGE_FORCE; break;
		case 'h': fn_headers = strdup(optarg); break;
		case 'n': is_by_qname = 1; break;
		case '1': flag |= MERGE_LEVEL1; break;
		case 'u': flag |= MERGE_UNCOMP; break;
		case 'R': reg = strdup(optarg); break;
        case 'o': oname = optarg; break;
        case 'i': fn_index = optarg; break;
        case 'x': fn_xstat = optarg; break;
        case 'c': fn_cstat = optarg; break;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "\n");
        if( vanilla ) fprintf(stderr, "Usage:   %s merge [-nru] [-h inh.sam] <out.bam> <in1.bam> <in2.bam> [...]\n\n", invocation_name);
        else          fprintf(stderr, "Usage:   %s merge [-nru] [-o out.bam] [-h inh.sam] <in1.bam> <in2.bam> [...]\n\n", invocation_name);
		fprintf(stderr, "Options: -n       sort by read names\n");
		fprintf(stderr, "         -r       attach RG tag (inferred from file names)\n");
		fprintf(stderr, "         -u       uncompressed BAM output\n");
        if( !vanilla ) {
            fprintf(stderr, "         -o FILE  write to FILE [stdout]\n");
            fprintf(stderr, "         -f       overwrite the output BAM if exist\n");
            fprintf(stderr, "         -i FILE  also write an index to FILE\n");
            fprintf(stderr, "         -x FILE  also perform `flagstatx' and write to FILE\n");
            fprintf(stderr, "         -c FILE  also perform `covstat' and write to FILE\n");
        }
		fprintf(stderr, "         -1       compress level 1\n");
		fprintf(stderr, "         -R STR   merge file in the specified region STR [all]\n");
		fprintf(stderr, "         -h FILE  copy the header in FILE to <out.bam> [in1.bam]\n\n");
		fprintf(stderr, "Note: Samtools' merge does not reconstruct the @RG dictionary in the header. Users\n");
		fprintf(stderr, "      must provide the correct header with -h, or uses Picard which properly maintains\n");
		fprintf(stderr, "      the header dictionary in merging.\n\n");
		return 1;
	}
    if( vanilla ) {
        if (bam_merge_core(is_by_qname, argv[optind], fn_headers, argc - optind - 1, argv + optind + 1, flag, reg) < 0) ret = 1;
    }
    else {
        if (!(flag & MERGE_FORCE) && strcmp(oname, "-") && !access(oname, F_OK)) {
            fprintf(stderr, "[%s] File '%s' exists. Please apply '-f' to overwrite. Abort.\n", __func__, oname);
            return 1;
        }
        // if (bam_merge_core(is_by_qname, oname, fn_headers, argc - optind, argv + optind, flag, reg) < 0) ret = 1;

        struct bam_sink fpout;
        bam_sink_init_file(&fpout, oname, flag);
        if( fn_cstat ) covstat_init(&fpout.ca);
        if( fn_xstat ) flagstatx_init(&fpout.fa);
        if( fn_index ) index_acc_init_A(&fpout.ia);

        int r = bam_merge_core_ext(is_by_qname, &fpout, fn_headers, argc - optind, argv + optind, flag, reg);
        if( fn_cstat ) {
            FILE *fp = fopen( fn_cstat, "w" );
            if( !fp ) { 
                fprintf(stderr, "[%s] Cannot write file `%s': %s\n", __func__, fn_cstat, strerror(errno));
                return 1;
            }
            covstat_print( &fpout.ca, fp, fpout.hdr ) ;
            fclose(fp) ;
        }
        if( fn_xstat ) {
            FILE *fp = fopen( fn_xstat, "w" );
            if( !fp ) { 
                fprintf(stderr, "[%s] Cannot write file `%s': %s\n", __func__, fn_xstat, strerror(errno));
                return 1;
            }
            flagstatx_print( &fpout.fa, fp ) ;
            fclose(fp) ;
        }
        if( fn_index && fpout.ia.idx ) {
            FILE *fp = fopen( fn_index, "wb" );
            if( !fp ) { 
                fprintf(stderr, "[%s] Cannot write file `%s': %s\n", __func__, fn_index, strerror(errno));
                return 1;
            }
            bam_index_t *idx = index_acc_finish(&fpout.ia, bam_tell(fpout.fp)) ;
            bam_index_save(idx, fp);
            fclose(fp) ;
        }
        bam_sink_close(&fpout);
        return r < 1 ;
    }
	free(reg);
	free(fn_headers);
	return ret;
}

typedef bam1_t *bam1_p;

static inline int bam1_lt(const bam1_p a, const bam1_p b)
{
	if (g_is_by_qname) {
		int t = strnum_cmp(bam1_qname(a), bam1_qname(b));
        return t > 0 ? 0 :
               t < 0 ? 1 :
               (a->core.flag & BAM_FPAIRED) < (b->core.flag & BAM_FPAIRED) ? 0 :
               (a->core.flag & BAM_FPAIRED) > (b->core.flag & BAM_FPAIRED) ? 1 :
               (a->core.flag & (BAM_FREAD1|BAM_FREAD2)) < (b->core.flag & (BAM_FREAD1|BAM_FREAD2)) ;
	} else return (((uint64_t)a->core.tid<<32|(a->core.pos+1)) < ((uint64_t)b->core.tid<<32|(b->core.pos+1)));
}
KSORT_INIT(sort, bam1_p, bam1_lt)

static void sort_blocks_ex(int n, int k, bam1_p *buf, struct bam_sink *sink, bam_header_t *h)
{
	int i;
	ks_mergesort(sort, k, buf, 0);
	bam_put_header(sink,h);
	for (i = 0; i < k; ++i)
		bam_put_rec(sink, buf[i]);
}

static void sort_blocks(int n, int k, bam1_p *buf, const char *prefix, bam_header_t *h, int is_stdout)
{
    struct bam_sink fpout;
    int flag=0;
	char *name = (char*)calloc(strlen(prefix) + 20, 1);
	if (n >= 0) {
		sprintf(name, "%s.%.4d.bam", prefix, n);
        flag |= MERGE_LEVEL1;
	} else {
		sprintf(name, "%s.bam", prefix);
	}
    bam_sink_init_file(&fpout, is_stdout?"-":name, flag);
	free(name);

	if (fpout.fp == 0) {
		fprintf(stderr, "[sort_blocks] fail to create file %s.\n", name);
		exit(1);
	}
    sort_blocks_ex(n,k,buf,&fpout,h);
    bam_sink_close(&fpout);
}

/*!
  @abstract Sort an unsorted BAM file based on the chromosome order
  and the leftmost position of an alignment

  @param  is_by_qname whether to sort by query name
  @param  fn       name of the file to be sorted
  @param  prefix   prefix of the output and the temporary files; upon
	                   sucessess, prefix.bam will be written.
  @param  max_mem  approxiate maximum memory (very inaccurate)

  @discussion It may create multiple temporary subalignment files
  and then merge them by calling bam_merge_core(). This function is
  NOT thread safe.
 */
void bam_sort_core_ext(int is_by_qname, const char *fn, const char *prefix, size_t max_mem, int is_stdout)
{
	int n, ret, k, i;
	size_t mem;
	bam_header_t *header;
	bamFile fp;
	bam1_t *b, **buf;

	g_is_by_qname = is_by_qname;
	n = k = 0; mem = 0;
	fp = strcmp(fn, "-")? bam_open(fn, "r") : bam_dopen(fileno(stdin), "r");
	if (fp == 0) {
		fprintf(stderr, "[bam_sort_core] fail to open file %s\n", fn);
		return;
	}
	header = bam_header_read(fp);
	buf = (bam1_t**)calloc(max_mem / BAM_CORE_SIZE, sizeof(bam1_t*));
	// write sub files
	for (;;) {
		if (buf[k] == 0) buf[k] = (bam1_t*)calloc(1, sizeof(bam1_t));
		b = buf[k];
		if ((ret = bam_read1(fp, b)) < 0) break;
		mem += ret;
		++k;
		if (mem >= max_mem) {
			sort_blocks(n++, k, buf, prefix, bam_header_dup(header), 0);
			mem = 0; k = 0;
		}
	}
	if (ret != -1)
		fprintf(stderr, "[bam_sort_core] truncated file. Continue anyway.\n");
	if (n == 0) sort_blocks(-1, k, buf, prefix, header, is_stdout);
	else { // then merge
		char **fns, *fnout;
		fprintf(stderr, "[bam_sort_core] merging from %d files...\n", n+1);
		sort_blocks(n++, k, buf, prefix, header, 0);
		fnout = (char*)calloc(strlen(prefix) + 20, 1);
		if (is_stdout) sprintf(fnout, "-");
		else sprintf(fnout, "%s.bam", prefix);
		fns = (char**)calloc(n, sizeof(char*));
		for (i = 0; i < n; ++i) {
			fns[i] = (char*)calloc(strlen(prefix) + 20, 1);
			sprintf(fns[i], "%s.%.4d.bam", prefix, i);
		}
		bam_merge_core(is_by_qname, fnout, 0, n, fns, 0, 0);
		free(fnout);
		for (i = 0; i < n; ++i) {
			unlink(fns[i]);
			free(fns[i]);
		}
		free(fns);
	}
	for (k = 0; k < max_mem / BAM_CORE_SIZE; ++k) {
		if (buf[k]) {
			free(buf[k]->data);
			free(buf[k]);
		}
	}
	free(buf);
	bam_close(fp);
}

void bam_sort_core(int is_by_qname, const char *fn, const char *prefix, size_t max_mem)
{
	bam_sort_core_ext(is_by_qname, fn, prefix, max_mem, 0);
}

int bam_sort(int argc, char *argv[])
{
	size_t max_mem = 500000000;
	int c, is_by_qname = 0, is_stdout = 0;
    char *ep ;
	while ((c = getopt(argc, argv, "nowm:")) >= 0) {
		switch (c) {
		case 'o': is_stdout = 1; break;
		case 'n': is_by_qname = 1; break;
        case 'w': g_ignore_warts = 1; break;                  
        case 'm': max_mem = strtol(optarg, &ep, 10);
                  switch(*ep) {
                      case 'k': max_mem <<= 10 ; ++ep ; break ;
                      case 'M': max_mem <<= 20 ; ++ep ; break ;
                      case 'G': max_mem <<= 30 ; ++ep ; break ;
                  }
                  if(!*ep) break;
        default: goto usage;
		}
	}
    if (optind + 2 > argc) goto usage ;
	bam_sort_core_ext(is_by_qname, argv[optind], argv[optind+1], max_mem, is_stdout);
	return 0;

usage:
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage:   %s sort [-on] [-m <maxMem>] <in.bam> <out.prefix>\n", invocation_name);
    fprintf(stderr, "Options: -n       sort by read names\n");
    fprintf(stderr, "         -o       write output to stdout\n");
    fprintf(stderr, "         -m NUM   use NUM bytes of memory (%ld%c)\n", (long)(
        max_mem >> 31 ? max_mem >> 30 : max_mem >> 22 ? max_mem >> 20 : max_mem >> 10),
        max_mem >> 31 ?           'G' : max_mem >> 22 ?           'M' :           'k' ) ;
    return 1;
}
