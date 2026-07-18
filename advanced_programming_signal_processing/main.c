#include "imageUtil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#ifdef _OPENMP
#include <omp.h>
#endif

/*
 * ============================================================================
 * 変更概要 (アルゴリズム・正答率・正規化・thresholdは一切不変)
 * ----------------------------------------------------------------------------
 * 旧: matching <image> <template> <rotation> <threshold> <opt> <out_dir> <mode>
 *     を「テンプレート1枚ごとに」プロセス起動していた。
 *
 * 新: matching <image> <template_dir> <rotation> <threshold> <opt> <out_dir> <mode>
 *     1プロセスで
 *       - 入力画像を1回だけ readPXM
 *       - template_dir 内の全 .ppm を読み込み Image*[] として保持
 *       - (isGray時) グレー変換済みテンプレートも前計算して保持
 *       - テンプレート単位で OpenMP 並列 (schedule(dynamic))
 *     マッチング関数内部の #pragma omp は削除し「直列」にした。
 *       → ネスト並列を避け、テンプレート方向で負荷分散する。
 *
 * SSD計算・早期打ち切り・seed_cutoffプレスキャン・正規化式・採用位置(最小y,x優先)は
 * 旧コードと完全に同一。結果は不変。
 * ============================================================================
 */

/* threshold から SSD 上限を逆算する。
 * 結果txtに書かれるのは distance < threshold の場合のみなので、
 * 「SSDがこの上限以上 → distanceがthreshold以上 → どうせ書かれない」位置は
 * 探索開始時から打ち切れる。真の最小が threshold 未満なら SSD < 上限 なので
 * 絶対に刈られない = txt出力・採用位置は厳密に不変。
 *   normMode 0 (simple, gray/color共通): dist = sqrt(SSD)/(w*h) < th ⟺ SSD < (th*w*h)^2
 *   normMode 1 (tN, gray):              dist = sqrt(SSD/tN)     < th ⟺ SSD < th^2*tN
 *   normMode 1 (tN, color):             dist = sqrt(SSD/(3tN))  < th ⟺ SSD < th^2*3*tN
 * +1 して「SSD == 境界値ちょうど」を誤って刈らないようにする。 */
static long long thresholdBound(double th, int normMode, int channels, int wh, int tN)
{
	double b;
	if (normMode == 0)
	{
		double s = th * (double)wh;
		b = s * s;
	}
	else
	{
		b = th * th * (double)channels * (double)(tN == 0 ? 1 : tN);
	}
	if (b >= 9.0e18) /* オーバーフロー防止: 事実上の無制限 */
		return LLONG_MAX;
	return (long long)b + 1;
}

/* --- プロファイリング (MATCH_PROF=1 のときだけ stderr に出力) --- */
static double nowSec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}
static int profEnabled(void)
{
	static int flag = -1;
	if (flag < 0)
	{
		const char *e = getenv("MATCH_PROF");
		flag = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return flag;
}

/* 識別力順ソート用: SSDは可換和なので走査順を変えても最終値は不変。
 * ミスマッチ窓に対する画素の期待差分は (t-μ)² に比例するため、
 * スコア降順に走査すると早期打ち切りが前倒しになる(結果不変)。 */
typedef struct { long long score; int idx; } ScoreIdx;
static int cmpScoreDesc(const void *a, const void *b)
{
	const ScoreIdx *sa = (const ScoreIdx *)a;
	const ScoreIdx *sb = (const ScoreIdx *)b;
	if (sa->score > sb->score) return -1;
	if (sa->score < sb->score) return 1;
	return sa->idx - sb->idx; /* 同点は元の順で安定化 */
}

/* edgeモード用: 有効画素の行内連続区間 (ランレングス)。
 * 内側ループが3*len バイトの連続走査になりSIMDが効く。 */
typedef struct { int srel; int trel; int len; long long score; } Run;
static int cmpRunMeanDesc(const void *a, const void *b)
{
	const Run *ra = (const Run *)a;
	const Run *rb = (const Run *)b;
	/* 平均スコア降順: score/len の比較を乗算で (len>0) */
	long long lhs = ra->score * (long long)rb->len;
	long long rhs = rb->score * (long long)ra->len;
	if (lhs > rhs) return -1;
	if (lhs < rhs) return 1;
	return ra->trel - rb->trel; /* 同点は元の順で安定化 */
}

/* ---- 直列版マッチング (旧templateMatchingGrayから #pragma omp を除去しただけ) ---- */
void templateMatchingGray(Image *src, Image *template, Point *position, double *distance, int isEdge, int normMode, double threshold)
{
	if (src->channel != 1 || template->channel != 1)
	{
		fprintf(stderr, "src and/or templeta image is not a gray image.\n");
		return;
	}

	if (template->width > src->width || template->height > src->height)
	{
		fprintf(stderr, "template (%dx%d) is larger than src (%dx%d); skipped.\n",
		        template->width, template->height, src->width, src->height);
		position->x = 0;
		position->y = 0;
		*distance = 1e30;
		return;
	}

	long long min_distance = LLONG_MAX;
	int ret_x = 0;
	int ret_y = 0;
	int i, j;
	double prof_t0 = profEnabled() ? nowSec() : 0.0;
	double prof_tpre = 0.0;
	long long prof_npos = 0, prof_nprune = 0, prof_rows = 0;

	int tN = 0;
	if (isEdge)
	{
		for (j = 0; j < template->height; j++)
			for (i = 0; i < template->width; i++)
				if (template->data[j * template->width + i] != 0)
					tN++;
	}
	else
	{
		tN = template->width * template->height;
	}

	/* threshold由来の打ち切り上限 (結果不変) */
	long long th_bound = thresholdBound(threshold, normMode, 1,
	                                    template->width * template->height, tN);

	/* --- ノルム下界プルーニング用の前計算 (非edgeのみ、カラー版と同じ論法で結果不変) --- */
	long long *sat = NULL;
	double sqrt_tsq = 0.0;
	if (!isEdge)
	{
		int W = src->width, H = src->height;
		sat = (long long *)malloc(sizeof(long long) * (size_t)(W + 1) * (H + 1));
		for (int xx = 0; xx <= W; xx++) sat[xx] = 0;
		for (int yy = 1; yy <= H; yy++)
		{
			long long rowsum = 0;
			sat[(size_t)yy * (W + 1)] = 0;
			for (int xx = 1; xx <= W; xx++)
			{
				int v = src->data[(yy - 1) * W + (xx - 1)];
				rowsum += (long long)v * v;
				sat[(size_t)yy * (W + 1) + xx] = sat[(size_t)(yy - 1) * (W + 1) + xx] + rowsum;
			}
		}
		long long tsq = 0;
		for (j = 0; j < template->height; j++)
			for (i = 0; i < template->width; i++)
			{
				int v = template->data[j * template->width + i];
				tsq += (long long)v * v;
			}
		sqrt_tsq = sqrt((double)tsq);
	}

	/* --- 識別力順の行並べ替え (結果不変) --- */
	int *roword = (int *)malloc(sizeof(int) * template->height);
	{
		double mu;
		{
			long long s = 0;
			int n = src->width * src->height;
			for (int p = 0; p < n; p++) s += src->data[p];
			mu = (double)s / (n > 0 ? n : 1);
		}
		ScoreIdx *si = (ScoreIdx *)malloc(sizeof(ScoreIdx) * template->height);
		for (j = 0; j < template->height; j++)
		{
			long long sc = 0;
			for (i = 0; i < template->width; i++)
			{
				double dv = (double)template->data[j * template->width + i] - mu;
				sc += (long long)(dv * dv);
			}
			si[j].score = sc;
			si[j].idx = j;
		}
		qsort(si, template->height, sizeof(ScoreIdx), cmpScoreDesc);
		for (j = 0; j < template->height; j++) roword[j] = si[j].idx;
		free(si);
	}

	/* --- 粗いプレスキャンで打ち切り種を作る (カラー版と同じ方針。結果不変) --- */
	long long seed_bound;
	{
		int ymax = src->height - template->height;
		int xmax = src->width - template->width;
		int step = template->width / 4;
		if (step < 1) step = 1;
		long long ps_min = th_bound;
		for (int py = 0; py <= ymax; py += step)
		{
			for (int px = 0; px <= xmax; px += step)
			{
				long long d = 0;
				for (int jo = 0; jo < template->height; jo++)
				{
					int jj = roword[jo];
					int base_s = (py + jj) * src->width + px;
					int base_t = jj * template->width;
					int rowacc = 0;
					for (int ii = 0; ii < template->width; ii++)
					{
						if (isEdge && template->data[base_t + ii] == 0)
							continue;
						int v = src->data[base_s + ii] - template->data[base_t + ii];
						rowacc += v * v;
					}
					d += rowacc;
					if (d >= ps_min) break;
				}
				if (d < ps_min) ps_min = d;
			}
		}
		seed_bound = (ps_min == LLONG_MAX) ? LLONG_MAX : ps_min + 1;
	}
	if (profEnabled()) prof_tpre = nowSec();

	/* ノルム下界の整数しきい値キャッシュ (cutoff変化時のみ再計算) */
	long long nb_cut = -1, nb_hi = LLONG_MAX, nb_lo = -1;

	for (int y = 0; y < (src->height - template->height); y++)
	{
		for (int x = 0; x < src->width - template->width; x++)
		{
			long long distance = 0;
			int stop = 0;
			long long cutoff = min_distance;
			if (seed_bound < cutoff) cutoff = seed_bound;
			if (th_bound < cutoff) cutoff = th_bound;

			/* ノルム下界 (整数比較版)。しきい値は cutoff 変化時のみ再計算。結果不変 */
			if (!isEdge)
			{
				prof_npos++;
				if (cutoff != nb_cut)
				{
					nb_cut = cutoff;
					double sc = sqrt((double)cutoff);
					double hs = sqrt_tsq + sc;
					double hv = hs * hs * (1.0 + 1e-9);
					nb_hi = (hv >= 9.0e18) ? LLONG_MAX : (long long)hv + 1;
					double ds = sqrt_tsq - sc;
					if (ds > 0.0)
					{
						double lv = ds * ds * (1.0 - 1e-9);
						nb_lo = (long long)lv - 1;
					}
					else
						nb_lo = -1;
				}
				int W1 = src->width + 1;
				long long a = sat[(size_t)(y + template->height) * W1 + (x + template->width)]
				            - sat[(size_t)y * W1 + (x + template->width)]
				            - sat[(size_t)(y + template->height) * W1 + x]
				            + sat[(size_t)y * W1 + x];
				if (a >= nb_hi || a <= nb_lo)
				{
					prof_nprune++;
					continue;
				}
			}

			if (!isEdge)
			{
				for (int jo = 0; jo < template->height; jo++)
				{
					int jj = roword[jo];
					int base_s = (y + jj) * src->width + x;
					int base_t = jj * template->width;
					int rowacc = 0; /* 行内部分和は32bit(SIMD化のため) */
					for (int ii = 0; ii < template->width; ii++)
					{
						int v = src->data[base_s + ii] - template->data[base_t + ii];
						rowacc += v * v;
					}
					distance += rowacc;
					prof_rows++;
					if (distance >= cutoff)
					{
						stop = 1;
						break;
					}
				}
			}
			else
			{
				for (int jj = 0; jj < template->height; jj++)
				{
					for (int ii = 0; ii < template->width; ii++)
					{
						if (template->data[jj * template->width + ii] == 0)
							continue;
						int v = (src->data[(y + jj) * src->width + (x + ii)] - template->data[jj * template->width + ii]);
						distance += v * v;
						if (distance >= cutoff)
						{
							stop = 1;
							break;
						}
					}
					if (stop)
						break;
				}
			}
			if (!stop && distance < min_distance)
			{
				min_distance = distance;
				ret_x = x;
				ret_y = y;
			}
		}
	}

	position->x = ret_x;
	position->y = ret_y;

	if (profEnabled())
	{
		double te = nowSec();
		fprintf(stderr, "PROF gray  tpl=%dx%d edge=%d setup+prescan=%.3fs main=%.3fs total=%.3fs pos=%lld norm_pruned=%lld rows=%lld\n",
		        template->width, template->height, isEdge,
		        prof_tpre - prof_t0, te - prof_tpre, te - prof_t0,
		        prof_npos, prof_nprune, prof_rows);
	}

	free(sat);
	free(roword);

	if (normMode == 0)
		*distance = sqrt((double)min_distance) / (template->width * template->height);
	else
		*distance = sqrt((double)min_distance / (tN == 0 ? 1 : tN));
}

/* ---- 直列版マッチング (旧templateMatchingColorから #pragma omp を除去しただけ) ---- */
void templateMatchingColor(Image *src, Image *template, Point *position, double *distance, int isEdge, int normMode, double threshold)
{
	if (src->channel != 3 || template->channel != 3)
	{
		fprintf(stderr, "src and/or templeta image is not a color image.\n");
		return;
	}

	if (template->width > src->width || template->height > src->height)
	{
		fprintf(stderr, "template (%dx%d) is larger than src (%dx%d); skipped.\n",
		        template->width, template->height, src->width, src->height);
		position->x = 0;
		position->y = 0;
		*distance = 1e30;
		return;
	}

	long long min_distance = LLONG_MAX;
	long long seed_cutoff = LLONG_MAX;
	double prof_t0 = profEnabled() ? nowSec() : 0.0;
	double prof_tpre = 0.0;
	long long prof_npos = 0, prof_nprune = 0, prof_rows = 0;
	int ret_x = 0;
	int ret_y = 0;
	int i, j;

	int tN = 0;
	if (isEdge)
	{
		for (j = 0; j < template->height; j++)
			for (i = 0; i < template->width; i++)
			{
				int pt2 = 3 * (j * template->width + i);
				if (!(template->data[pt2 + 0] == 0 && template->data[pt2 + 1] == 0 && template->data[pt2 + 2] == 0))
					tN++;
			}
	}
	else
	{
		tN = template->width * template->height;
	}

	/* threshold由来の打ち切り上限 (結果不変) */
	long long th_bound = thresholdBound(threshold, normMode, 3,
	                                    template->width * template->height, tN);

	/* --- edgeモード用: 非黒画素の行内連続区間(ラン)を1回だけ前計算 ---
	 * 3*((y+j)*W + (x+i)) = 3*(y*W+x) + 3*(j*W+i) の分解は画素リスト版と同じ。
	 * ランにすることで内側ループが連続メモリ走査になりベクトル化される。
	 * 項の集合は同一なのでSSD値は厳密に不変。 */
	Run *runs = NULL;
	int nruns = 0;
	if (isEdge)
	{
		int rcap = 64;
		runs = (Run *)malloc(sizeof(Run) * rcap);
		for (j = 0; j < template->height; j++)
		{
			i = 0;
			while (i < template->width)
			{
				int pt2 = 3 * (j * template->width + i);
				if (template->data[pt2 + 0] == 0 && template->data[pt2 + 1] == 0 && template->data[pt2 + 2] == 0)
				{
					i++;
					continue;
				}
				int start = i;
				while (i < template->width)
				{
					int q = 3 * (j * template->width + i);
					if (template->data[q + 0] == 0 && template->data[q + 1] == 0 && template->data[q + 2] == 0)
						break;
					i++;
				}
				if (nruns == rcap) { rcap *= 2; runs = (Run *)realloc(runs, sizeof(Run) * rcap); }
				runs[nruns].srel = 3 * (j * src->width + start);
				runs[nruns].trel = 3 * (j * template->width + start);
				runs[nruns].len = i - start;
				runs[nruns].score = 0;
				nruns++;
			}
		}
	}

	/* --- 識別力スコア用の画像平均 (1ch平均で十分) --- */
	double mu;
	{
		long long s = 0;
		long long n = (long long)src->width * src->height * 3;
		for (long long p = 0; p < n; p++) s += src->data[p];
		mu = (double)s / (n > 0 ? n : 1);
	}

	/* --- 識別力順の行並べ替え (非edge用。結果不変) --- */
	int *roword = (int *)malloc(sizeof(int) * template->height);
	{
		ScoreIdx *si = (ScoreIdx *)malloc(sizeof(ScoreIdx) * template->height);
		for (j = 0; j < template->height; j++)
		{
			long long sc = 0;
			for (i = 0; i < template->width; i++)
			{
				int pt2 = 3 * (j * template->width + i);
				for (int c = 0; c < 3; c++)
				{
					double dv = (double)template->data[pt2 + c] - mu;
					sc += (long long)(dv * dv);
				}
			}
			si[j].score = sc;
			si[j].idx = j;
		}
		qsort(si, template->height, sizeof(ScoreIdx), cmpScoreDesc);
		for (j = 0; j < template->height; j++) roword[j] = si[j].idx;
		free(si);
	}

	/* --- edgeモード: ランを識別力(平均スコア)降順にソート (結果不変) --- */
	if (isEdge && nruns > 0)
	{
		for (int r = 0; r < nruns; r++)
		{
			long long sc = 0;
			for (int p = 0; p < 3 * runs[r].len; p++)
			{
				double dv = (double)template->data[runs[r].trel + p] - mu;
				sc += (long long)(dv * dv);
			}
			runs[r].score = sc;
		}
		qsort(runs, nruns, sizeof(Run), cmpRunMeanDesc);
	}

	/* --- ノルム下界プルーニング用の前計算 (非edgeのみ、結果不変) ---
	 * Cauchy-Schwarz: SSD = Σs²+Σt²-2Σst ≥ (√Σs²-√Σt²)²
	 * Σt² はここで1回計算。Σs²(窓内) は二乗値の積分画像から O(1) で引く。
	 * 下界 ≥ cutoff の位置は1画素も計算せずスキップできる。
	 * 整数の積分画像なので Σs² は厳密。sqrt の丸めは安全係数0.999999で
	 * 下界を僅かに縮めて吸収する(刈り損ねは計算量が増えるだけで結果不変)。 */
	long long *sat = NULL;
	double sqrt_tsq = 0.0;
	if (!isEdge)
	{
		int W = src->width, H = src->height;
		sat = (long long *)malloc(sizeof(long long) * (size_t)(W + 1) * (H + 1));
		for (int xx = 0; xx <= W; xx++) sat[xx] = 0;
		for (int yy = 1; yy <= H; yy++)
		{
			long long rowsum = 0;
			sat[(size_t)yy * (W + 1)] = 0;
			for (int xx = 1; xx <= W; xx++)
			{
				int pt = 3 * ((yy - 1) * W + (xx - 1));
				int r = src->data[pt + 0], g = src->data[pt + 1], b = src->data[pt + 2];
				rowsum += (long long)r * r + (long long)g * g + (long long)b * b;
				sat[(size_t)yy * (W + 1) + xx] = sat[(size_t)(yy - 1) * (W + 1) + xx] + rowsum;
			}
		}
		long long tsq = 0;
		for (j = 0; j < template->height; j++)
			for (i = 0; i < template->width; i++)
			{
				int pt2 = 3 * (j * template->width + i);
				int r = template->data[pt2 + 0], g = template->data[pt2 + 1], b = template->data[pt2 + 2];
				tsq += (long long)r * r + (long long)g * g + (long long)b * b;
			}
		sqrt_tsq = sqrt((double)tsq);
	}

	/* --- 粗いプレスキャンで打ち切り用の暫定minを求める --- */
	/* プレスキャンは閾値の種を作るだけで argmin 判定に使わないため、
	   th_bound での刈り込みを足しても結果は不変。 */
	{
		int ymax = src->height - template->height;
		int xmax = src->width - template->width;
		int step = template->width / 4;
		if (step < 1) step = 1;
		long long ps_min = th_bound; /* 種の初期値もthreshold上限から */
		for (int py = 0; py <= ymax; py += step)
		{
			for (int px = 0; px <= xmax; px += step)
			{
				long long d = 0;
				if (isEdge)
				{
					/* ラン走査 (連続メモリ) + ランごとの打ち切り判定 */
					int base = 3 * (py * src->width + px);
					for (int r = 0; r < nruns; r++)
					{
						const unsigned char *sp = src->data + base + runs[r].srel;
						const unsigned char *tp = template->data + runs[r].trel;
						int n3 = 3 * runs[r].len;
						int racc = 0;
						for (int p = 0; p < n3; p++)
						{
							int v = sp[p] - tp[p];
							racc += v * v;
						}
						d += racc;
						if (d >= ps_min) break;
					}
				}
				else
				{
					for (int jo = 0; jo < template->height; jo++)
					{
						int jj = roword[jo];
						int base_s = (py + jj) * src->width + px;
						int base_t = jj * template->width;
						int rowacc = 0;
						for (int ii = 0; ii < template->width; ii++)
						{
							int pt = 3 * (base_s + ii);
							int pt2 = 3 * (base_t + ii);
							int r = src->data[pt + 0] - template->data[pt2 + 0];
							int g = src->data[pt + 1] - template->data[pt2 + 1];
							int b = src->data[pt + 2] - template->data[pt2 + 2];
							rowacc += (r * r + g * g + b * b);
						}
						d += rowacc;
						if (d >= ps_min) break;
					}
				}
				if (d < ps_min) ps_min = d;
			}
		}
		seed_cutoff = ps_min;
	}

	long long seed_bound = (seed_cutoff == LLONG_MAX) ? LLONG_MAX : seed_cutoff + 1;
	if (profEnabled()) prof_tpre = nowSec();

	/* ノルム下界の整数しきい値キャッシュ (cutoff変化時のみ再計算) */
	long long nb_cut = -1, nb_hi = LLONG_MAX, nb_lo = -1;

	for (int y = 0; y < (src->height - template->height); y++)
	{
		for (int x = 0; x < src->width - template->width; x++)
		{
			long long distance = 0;
			int stop = 0;
			long long cutoff = min_distance;
			if (seed_bound < cutoff) cutoff = seed_bound;
			if (th_bound < cutoff) cutoff = th_bound;

			/* ノルム下界 (整数比較版): (√a-√b)² ≥ cutoff ⟺ a ≥ (√b+√c)² または a ≤ (√b-√c)²
			 * しきい値は cutoff 変化時のみ再計算。丸めは安全側(刈りすぎ不可)。結果不変 */
			if (!isEdge)
			{
				prof_npos++;
				if (cutoff != nb_cut)
				{
					nb_cut = cutoff;
					double sc = sqrt((double)cutoff);
					double hs = sqrt_tsq + sc;
					double hv = hs * hs * (1.0 + 1e-9);
					nb_hi = (hv >= 9.0e18) ? LLONG_MAX : (long long)hv + 1;
					double ds = sqrt_tsq - sc;
					if (ds > 0.0)
					{
						double lv = ds * ds * (1.0 - 1e-9);
						nb_lo = (long long)lv - 1;
					}
					else
						nb_lo = -1;
				}
				int W1 = src->width + 1;
				long long a = sat[(size_t)(y + template->height) * W1 + (x + template->width)]
				            - sat[(size_t)y * W1 + (x + template->width)]
				            - sat[(size_t)(y + template->height) * W1 + x]
				            + sat[(size_t)y * W1 + x];
				if (a >= nb_hi || a <= nb_lo)
				{
					prof_nprune++;
					continue;
				}
			}

			if (!isEdge)
			{
				for (int jo = 0; jo < template->height; jo++)
				{
					int jj = roword[jo];
					int base_s = (y + jj) * src->width + x;
					int base_t = jj * template->width;
					int rowacc = 0; /* 行内部分和は32bitで足す(SIMD化のため)。行末でllへ集約 */
					for (int ii = 0; ii < template->width; ii++)
					{
						int pt = 3 * (base_s + ii);
						int pt2 = 3 * (base_t + ii);
						int r = src->data[pt + 0] - template->data[pt2 + 0];
						int g = src->data[pt + 1] - template->data[pt2 + 1];
						int b = src->data[pt + 2] - template->data[pt2 + 2];
						rowacc += (r * r + g * g + b * b);
					}
					distance += rowacc;
					prof_rows++;
					if (distance >= cutoff)
					{
						stop = 1;
						break;
					}
				}
			}
			else
			{
				/* edgeモード: ソート済みランを連続走査。黒判定なし・SIMD可。
				 * 打ち切りはランごと(粗くなるだけで結果は不変)。 */
				prof_npos++;
				int base = 3 * (y * src->width + x);
				for (int r = 0; r < nruns; r++)
				{
					const unsigned char *sp = src->data + base + runs[r].srel;
					const unsigned char *tp = template->data + runs[r].trel;
					int n3 = 3 * runs[r].len;
					int racc = 0; /* ラン内部分和は32bit(SIMD化のため) */
					for (int p = 0; p < n3; p++)
					{
						int v = sp[p] - tp[p];
						racc += v * v;
					}
					distance += racc;
					prof_rows++;
					if (distance >= cutoff)
					{
						stop = 1;
						break;
					}
				}
			}
			if (!stop && distance < min_distance)
			{
				min_distance = distance;
				ret_x = x;
				ret_y = y;
			}
		}
	}

	position->x = ret_x;
	position->y = ret_y;

	if (profEnabled())
	{
		double te = nowSec();
		fprintf(stderr, "PROF color tpl=%dx%d edge=%d setup+prescan=%.3fs main=%.3fs total=%.3fs pos=%lld norm_pruned=%lld rows=%lld\n",
		        template->width, template->height, isEdge,
		        prof_tpre - prof_t0, te - prof_tpre, te - prof_t0,
		        prof_npos, prof_nprune, prof_rows);
	}

	free(runs);
	free(sat);
	free(roword);

	if (normMode == 0)
		*distance = sqrt((double)min_distance) / (template->width * template->height);
	else
		*distance = sqrt((double)min_distance / (3 * (tN == 0 ? 1 : tN)));
}

/* ============================================================================
 * テンプレートディレクトリ処理
 * ============================================================================ */

/* テンプレートファイル1件分の保持データ */
typedef struct {
	char *path;          /* フルパス (free対象) */
	Image *img;          /* 読み込んだ元画像 (free対象) */
	Image *gray;         /* isGray時のグレー変換済み。非isGrayならNULL (free対象) */
} TemplateEntry;

/* ファイル名末尾が .ppm か判定 */
static int hasPpmExt(const char *name)
{
	size_t n = strlen(name);
	return (n >= 4 && strcmp(name + n - 4, ".ppm") == 0);
}

/* qsort用: パス文字列で昇順。旧スクリプトの glob 展開順(辞書順)に合わせて再現性を持たせる */
static int cmpEntry(const void *a, const void *b)
{
	const TemplateEntry *ea = (const TemplateEntry *)a;
	const TemplateEntry *eb = (const TemplateEntry *)b;
	return strcmp(ea->path, eb->path);
}

/* ============================================================================
 * バッチモード: ./matching -jobs <jobsfile>
 *   1行 = 1ジョブ: <image> <template_dir> <rotation> <threshold> <opt> <out_dir> <mode>
 *   全ジョブを1プロセスで処理する。
 *   - 画像・テンプレートディレクトリは1回だけ読み込みキャッシュ
 *   - (ジョブ×テンプレート) の全タスクにフラットに OpenMP 並列
 *   - 結果はジョブごとに配列へ貯め、最後にテンプレート辞書順で直列書き出し
 *     (旧挙動と同じ順序・同じclear/追記セマンティクス。結果は不変)
 *   - 'w' オプションはバッチでは未対応 (スクリプトは使用していない)
 * ============================================================================ */

typedef struct {
	int found;
	Point pos;
	int w, h;
	double dist;
	char *tname;
} BRes;

typedef struct {
	char image[512], tdir[512], opt[32], outdir[256], mode[16];
	int rotation;
	double threshold;
	int normMode, isEdge, isGray, doClear, isPrint;
	int imgIdx, dirIdx;
	BRes *res;
} BJob;

/* --- 画像キャッシュ --- */
typedef struct { char *path; Image *img; Image *gray; } CImg;
static CImg *g_imgs = NULL;
static int g_nimgs = 0, g_capimgs = 0;

static int cacheImage(const char *path)
{
	for (int k = 0; k < g_nimgs; k++)
		if (strcmp(g_imgs[k].path, path) == 0) return k;
	Image *im = readPXM(path);
	if (im == NULL || im->width <= 0 || im->height <= 0 || im->data == NULL)
	{
		fprintf(stderr, "batch: failed to read image %s\n", path);
		if (im) freeImage(im);
		return -1;
	}
	if (g_nimgs == g_capimgs)
	{
		g_capimgs = g_capimgs ? g_capimgs * 2 : 32;
		g_imgs = (CImg *)realloc(g_imgs, sizeof(CImg) * g_capimgs);
	}
	g_imgs[g_nimgs].path = strdup(path);
	g_imgs[g_nimgs].img = im;
	g_imgs[g_nimgs].gray = NULL;
	return g_nimgs++;
}

static void ensureImageGray(int idx)
{
	if (g_imgs[idx].gray != NULL) return;
	Image *im = g_imgs[idx].img;
	if (im->channel != 3) return;
	Image *gy = createImage(im->width, im->height, 1);
	cvtColorGray(im, gy);
	g_imgs[idx].gray = gy;
}

/* --- テンプレートディレクトリキャッシュ --- */
typedef struct { char *dir; int n; TemplateEntry *tpl; int hasGray; } CDir;
static CDir *g_dirs = NULL;
static int g_ndirs = 0, g_capdirs = 0;

static int cacheDir(const char *dirpath)
{
	for (int k = 0; k < g_ndirs; k++)
		if (strcmp(g_dirs[k].dir, dirpath) == 0) return k;

	DIR *dp = opendir(dirpath);
	if (dp == NULL)
	{
		fprintf(stderr, "batch: cannot open template dir %s\n", dirpath);
		return -1;
	}
	int cap = 16, n = 0;
	TemplateEntry *tpl = (TemplateEntry *)malloc(sizeof(TemplateEntry) * cap);
	struct dirent *de;
	while ((de = readdir(dp)) != NULL)
	{
		if (!hasPpmExt(de->d_name)) continue;
		if (n == cap) { cap *= 2; tpl = (TemplateEntry *)realloc(tpl, sizeof(TemplateEntry) * cap); }
		size_t plen = strlen(dirpath) + 1 + strlen(de->d_name) + 1;
		char *path = (char *)malloc(plen);
		snprintf(path, plen, "%s/%s", dirpath, de->d_name);
		Image *timg = readPXM(path);
		if (timg == NULL || timg->width <= 0 || timg->height <= 0 || timg->data == NULL)
		{
			fprintf(stderr, "batch: skip corrupt template %s\n", path);
			if (timg) freeImage(timg);
			free(path);
			continue;
		}
		tpl[n].path = path;
		tpl[n].img = timg;
		tpl[n].gray = NULL;
		n++;
	}
	closedir(dp);
	qsort(tpl, n, sizeof(TemplateEntry), cmpEntry); /* glob(辞書)順で再現性を保つ */

	if (g_ndirs == g_capdirs)
	{
		g_capdirs = g_capdirs ? g_capdirs * 2 : 8;
		g_dirs = (CDir *)realloc(g_dirs, sizeof(CDir) * g_capdirs);
	}
	g_dirs[g_ndirs].dir = strdup(dirpath);
	g_dirs[g_ndirs].n = n;
	g_dirs[g_ndirs].tpl = tpl;
	g_dirs[g_ndirs].hasGray = 0;
	return g_ndirs++;
}

static void ensureDirGray(int idx)
{
	if (g_dirs[idx].hasGray) return;
	for (int t = 0; t < g_dirs[idx].n; t++)
	{
		Image *im = g_dirs[idx].tpl[t].img;
		if (g_dirs[idx].tpl[t].gray == NULL && im->channel == 3)
		{
			Image *gy = createImage(im->width, im->height, 1);
			cvtColorGray(im, gy);
			g_dirs[idx].tpl[t].gray = gy;
		}
	}
	g_dirs[idx].hasGray = 1;
}

/* 出力txtパス生成 (単発モードと同一規則: base名から _xxx マークを除去) */
static void makeTxtPath(const char *input_file, const char *outdir, char *out, size_t outsz)
{
	char *ibase = getBaseName(input_file); /* リーク許容(旧仕様) */
	char *p;
	const char *marks[] = {"_none","_blur","_contrast","_edge","_resize50","_resize200",
	                       "_rotate90","_rotate180","_rotate270", NULL};
	for (int m = 0; marks[m]; m++)
		if ((p = strstr(ibase, marks[m])) != NULL) { *p = '\0'; break; }
	snprintf(out, outsz, "%s/%s.txt", outdir, ibase);
}

static int runJobs(const char *jobsfile)
{
	FILE *fp = fopen(jobsfile, "r");
	if (fp == NULL)
	{
		fprintf(stderr, "batch: cannot open jobs file %s\n", jobsfile);
		return -1;
	}

	int capj = 64, nj = 0;
	BJob *jobs = (BJob *)malloc(sizeof(BJob) * capj);
	char line[2048];
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		if (line[0] == '#' || line[0] == '\n') continue;
		if (nj == capj) { capj *= 2; jobs = (BJob *)realloc(jobs, sizeof(BJob) * capj); }
		BJob *jb = &jobs[nj];
		memset(jb, 0, sizeof(BJob));
		strcpy(jb->mode, "tN");
		int got = sscanf(line, "%511s %511s %d %lf %31s %255s %15s",
		                 jb->image, jb->tdir, &jb->rotation, &jb->threshold,
		                 jb->opt, jb->outdir, jb->mode);
		if (got < 6)
		{
			fprintf(stderr, "batch: bad job line skipped: %s", line);
			continue;
		}
		if (strcmp(jb->mode, "simple") == 0)      { jb->normMode = 0; jb->isEdge = 0; }
		else if (strcmp(jb->mode, "edge") == 0)   { jb->normMode = 1; jb->isEdge = 1; }
		else                                       { jb->normMode = 1; jb->isEdge = 0; }
		jb->isGray  = (strchr(jb->opt, 'g') != NULL);
		jb->doClear = (strchr(jb->opt, 'c') != NULL);
		jb->isPrint = (strchr(jb->opt, 'p') != NULL);
		if (strchr(jb->opt, 'w') != NULL)
			fprintf(stderr, "batch: 'w' option is not supported in batch mode (ignored)\n");
		nj++;
	}
	fclose(fp);

	/* --- 前ロード (直列): 並列区間中はキャッシュを読み取り専用にするため --- */
	for (int jn = 0; jn < nj; jn++)
	{
		jobs[jn].imgIdx = cacheImage(jobs[jn].image);
		jobs[jn].dirIdx = cacheDir(jobs[jn].tdir);
		if (jobs[jn].imgIdx < 0 || jobs[jn].dirIdx < 0)
		{
			fprintf(stderr, "batch: job %d disabled (load failure)\n", jn);
			continue;
		}
		if (jobs[jn].isGray)
		{
			ensureImageGray(jobs[jn].imgIdx);
			ensureDirGray(jobs[jn].dirIdx);
		}
		jobs[jn].res = (BRes *)calloc(g_dirs[jobs[jn].dirIdx].n, sizeof(BRes));
	}

	/* --- タスク列挙: (ジョブ, テンプレート) --- */
	typedef struct { int jn; int t; } Task;
	int ntask = 0;
	for (int jn = 0; jn < nj; jn++)
		if (jobs[jn].imgIdx >= 0 && jobs[jn].dirIdx >= 0)
			ntask += g_dirs[jobs[jn].dirIdx].n;
	Task *tasks = (Task *)malloc(sizeof(Task) * (ntask > 0 ? ntask : 1));
	{
		int k = 0;
		for (int jn = 0; jn < nj; jn++)
		{
			if (jobs[jn].imgIdx < 0 || jobs[jn].dirIdx < 0) continue;
			for (int t = 0; t < g_dirs[jobs[jn].dirIdx].n; t++)
			{
				tasks[k].jn = jn;
				tasks[k].t = t;
				k++;
			}
		}
	}

	/* --- 全タスクにフラットに並列 --- */
	#pragma omp parallel for schedule(dynamic)
	for (int k = 0; k < ntask; k++)
	{
		BJob *jb = &jobs[tasks[k].jn];
		TemplateEntry *te = &g_dirs[jb->dirIdx].tpl[tasks[k].t];
		Image *srcc = g_imgs[jb->imgIdx].img;
		Image *srcg = g_imgs[jb->imgIdx].gray;
		Point result = {0, 0};
		double distance = 0.0;

		if (jb->isGray && srcg != NULL && te->gray != NULL)
			templateMatchingGray(srcg, te->gray, &result, &distance, jb->isEdge, jb->normMode, jb->threshold);
		else
			templateMatchingColor(srcc, te->img, &result, &distance, jb->isEdge, jb->normMode, jb->threshold);

		BRes *r = &jb->res[tasks[k].t];
		r->found = (distance < jb->threshold);
		r->pos = result;
		r->w = te->img->width;
		r->h = te->img->height;
		r->dist = distance;
		r->tname = getBaseName(te->path);
	}

	/* --- 書き出し (直列。ジョブはファイル順、テンプレートは辞書順) --- */
	for (int jn = 0; jn < nj; jn++)
	{
		BJob *jb = &jobs[jn];
		if (jb->imgIdx < 0 || jb->dirIdx < 0) continue;
		char txt[800];
		makeTxtPath(jb->image, jb->outdir, txt, sizeof(txt));
		if (jb->doClear)
			clearResult(txt);
		for (int t = 0; t < g_dirs[jb->dirIdx].n; t++)
		{
			BRes *r = &jb->res[t];
			if (r->found)
			{
				writeResult(txt, r->tname, r->pos, r->w, r->h, jb->rotation, r->dist);
				if (jb->isPrint)
					printf("[Found    ] %s %d %d %d %d %d %f\n",
					       r->tname, r->pos.x, r->pos.y, r->w, r->h, jb->rotation, r->dist);
			}
			else if (jb->isPrint)
			{
				printf("[Not found] %s %d %d %d %d %d %f\n",
				       r->tname, r->pos.x, r->pos.y, r->w, r->h, jb->rotation, r->dist);
			}
		}
	}

	printf("batch: %d jobs, %d tasks done.\n", nj, ntask);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "-jobs") == 0)
		return runJobs(argv[2]);

	if (argc < 7)
	{
		fprintf(stderr, "Usage: %s <src_image> <template_dir> <rotation> <threshold> <option(c,w,p,g)> <output_dir> [mode]\n", argv[0]);
		fprintf(stderr, "  template_dir : directory containing *.ppm templates (processed all at once)\n");
		fprintf(stderr, "Option:\n c) clear txt result once at start.\n w) write result image.\n p) print results.\n g) convert to gray.\n");
		fprintf(stderr, "Mode (7th): simple | tN | edge   (omitted -> tN)\n");
		return -1;
	}

	char *input_file  = argv[1];
	char *template_dir = argv[2];
	int rotation      = atoi(argv[3]);
	double threshold  = atof(argv[4]);
	char *opt         = argv[5];
	char *output_dir  = argv[6];

	int isEdge = 0;
	int normMode = 1; /* default tN */
	if (argc >= 8)
	{
		if (strcmp(argv[7], "simple") == 0) { normMode = 0; isEdge = 0; }
		else if (strcmp(argv[7], "tN") == 0) { normMode = 1; isEdge = 0; }
		else if (strcmp(argv[7], "edge") == 0) { normMode = 1; isEdge = 1; }
	}

	int isWriteImageResult = (strchr(opt, 'w') != NULL);
	int isPrintResult      = (strchr(opt, 'p') != NULL);
	int isGray             = (strchr(opt, 'g') != NULL);
	int doClear            = (strchr(opt, 'c') != NULL);

	printf("rotation -> %d\n", rotation);
	printf("norm mode -> %s%s\n", normMode == 0 ? "simple" : "tN", isEdge ? " (edge skip)" : "");

	/* ---- 入力画像を1回だけ読む ---- */
	Image *img = readPXM(input_file);
	if (img == NULL || img->width <= 0 || img->height <= 0 || img->data == NULL)
	{
		fprintf(stderr, "Failed to read (or corrupt) input image: %s\n", input_file);
		if (img) freeImage(img);
		return -1;
	}

	/* isGray時: 入力画像のグレー版を1回だけ作る */
	Image *img_gray = NULL;
	if (isGray && img->channel == 3)
	{
		img_gray = createImage(img->width, img->height, 1);
		cvtColorGray(img, img_gray);
	}

	/* ---- 出力txtパス (旧コードと同じ: 入力画像のbase名から _xxx を除去) ---- */
	/* 注意: getBaseName は malloc して返し、途中ポインタを返すため free 不可(旧コードと同じくリーク許容) */
	char *ibase = getBaseName(input_file);
	{
		char *p;
		const char *marks[] = {"_none","_blur","_contrast","_edge","_resize50","_resize200",
		                       "_rotate90","_rotate180","_rotate270", NULL};
		for (int m = 0; marks[m]; m++)
			if ((p = strstr(ibase, marks[m])) != NULL) { *p = '\0'; break; }
	}
	char output_name_base[512];
	char output_name_txt[520];
	snprintf(output_name_base, sizeof(output_name_base), "%s/%s", output_dir, ibase);
	snprintf(output_name_txt, sizeof(output_name_txt), "%s.txt", output_name_base);

	/* ---- clear は「プロセス開始時に1回だけ」 ---- */
	if (doClear)
		clearResult(output_name_txt);

	/* ---- テンプレートディレクトリを列挙して全読み込み ---- */
	DIR *dp = opendir(template_dir);
	if (dp == NULL)
	{
		fprintf(stderr, "cannot open template dir: %s\n", template_dir);
		if (img_gray) freeImage(img_gray);
		freeImage(img);
		return -1;
	}

	int cap = 64, ntpl = 0;
	TemplateEntry *tpl = (TemplateEntry *)malloc(sizeof(TemplateEntry) * cap);

	struct dirent *de;
	while ((de = readdir(dp)) != NULL)
	{
		if (!hasPpmExt(de->d_name)) continue;
		if (ntpl == cap) { cap *= 2; tpl = (TemplateEntry *)realloc(tpl, sizeof(TemplateEntry) * cap); }

		size_t plen = strlen(template_dir) + 1 + strlen(de->d_name) + 1;
		char *path = (char *)malloc(plen);
		snprintf(path, plen, "%s/%s", template_dir, de->d_name);

		Image *timg = readPXM(path);
		if (timg == NULL || timg->width <= 0 || timg->height <= 0 || timg->data == NULL)
		{
			fprintf(stderr, "skip corrupt template: %s\n", path);
			if (timg) freeImage(timg);
			free(path);
			continue;
		}

		Image *tgray = NULL;
		if (isGray && timg->channel == 3)
		{
			tgray = createImage(timg->width, timg->height, 1);
			cvtColorGray(timg, tgray);
		}

		tpl[ntpl].path = path;
		tpl[ntpl].img  = timg;
		tpl[ntpl].gray = tgray;
		ntpl++;
	}
	closedir(dp);

	/* glob 展開順に合わせて辞書順ソート(採用位置の再現性のため) */
	qsort(tpl, ntpl, sizeof(TemplateEntry), cmpEntry);

	/* 各テンプレートの結果を一時保存 (writeResultはcriticalで直列化するが、
	   画像書き出しやprintの順序を安定させるため結果を配列に貯める) */
	typedef struct {
		int found;
		Point pos;
		int w, h;
		double dist;
		char *tname;   /* getBaseName(path) の結果 (リーク許容) */
	} MatchResult;
	MatchResult *res = (MatchResult *)calloc(ntpl, sizeof(MatchResult));

	Image *src_color = img;
	Image *src_gray  = img_gray;

	/* ---- テンプレート単位で並列化。内部関数は直列。 ---- */
	#pragma omp parallel for schedule(dynamic)
	for (int t = 0; t < ntpl; t++)
	{
		Point result = {0, 0};
		double distance = 0.0;
		Image *tm = tpl[t].img;

		if (isGray && src_gray != NULL && tpl[t].gray != NULL)
			templateMatchingGray(src_gray, tpl[t].gray, &result, &distance, isEdge, normMode, threshold);
		else
			templateMatchingColor(src_color, tm, &result, &distance, isEdge, normMode, threshold);

		res[t].found = (distance < threshold);
		res[t].pos   = result;
		res[t].w     = tm->width;
		res[t].h     = tm->height;
		res[t].dist  = distance;
		res[t].tname = getBaseName(tpl[t].path); /* 各スレッドで別mallocなので安全 */
	}

	/* ---- 結果の書き出し (直列。順序 = ソート済みテンプレート順) ---- */
	for (int t = 0; t < ntpl; t++)
	{
		if (res[t].found)
		{
			writeResult(output_name_txt, res[t].tname, res[t].pos, res[t].w, res[t].h, rotation, res[t].dist);
			if (isPrintResult)
				printf("[Found    ] %s %d %d %d %d %d %f\n",
				       res[t].tname, res[t].pos.x, res[t].pos.y, res[t].w, res[t].h, rotation, res[t].dist);
		}
		else if (isPrintResult)
		{
			printf("[Not found] %s %d %d %d %d %d %f\n",
			       res[t].tname, res[t].pos.x, res[t].pos.y, res[t].w, res[t].h, rotation, res[t].dist);
		}
	}

	/* ---- 画像書き出し(w指定時): 全テンプレートの枠を1枚に描くと旧挙動と変わるため、
	   旧コードは「最後に処理したテンプレートの枠」を上書き保存していた。
	   ここでは found のもの全てを描いて1枚保存する。w は主にデバッグ用途なので
	   スコア比較(txt)には影響しない。挙動を厳密に旧に合わせたい場合は要相談。 ---- */
	if (isWriteImageResult)
	{
		for (int t = 0; t < ntpl; t++)
			if (res[t].found)
				drawRectangle(img, res[t].pos, res[t].w, res[t].h);
		char output_name_img[520];
		if (img->channel == 3)
			snprintf(output_name_img, sizeof(output_name_img), "%s.ppm", output_name_base);
		else
			snprintf(output_name_img, sizeof(output_name_img), "%s.pgm", output_name_base);
		writePXM(output_name_img, img);
	}

	/* ---- 後始末 ---- */
	for (int t = 0; t < ntpl; t++)
	{
		freeImage(tpl[t].img);
		if (tpl[t].gray) freeImage(tpl[t].gray);
		free(tpl[t].path);
	}
	free(tpl);
	free(res);
	if (img_gray) freeImage(img_gray);
	freeImage(img);

	return 0;
}