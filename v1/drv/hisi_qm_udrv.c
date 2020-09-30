/*
 * Copyright 2018-2019 Huawei Technologies Co.,Ltd.All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/types.h>

#include "hisi_qm_udrv.h"
#include "hisi_zip_udrv.h"
#include "hisi_hpre_udrv.h"
#include "hisi_sec_udrv.h"


int qm_db_v1(struct qm_queue_info *q, __u8 cmd,
	       __u16 idx, __u8 priority)
{
	void *base = q->doorbell_base;
	__u16 sqn = q->sqn;
	__u64 doorbell;

	doorbell = (__u64)sqn | ((__u64)cmd << QM_DBELL_CMD_SHIFT);
	doorbell |= ((__u64)idx | ((__u64)priority << QM_DBELL_CMD_SHIFT)) <<
		    QM_DBELL_HLF_SHIFT;
	*((__u64 *)base) = doorbell;

	return 0;
}

static int qm_db_v2(struct qm_queue_info *q, __u8 cmd,
		      __u16 idx, __u8 priority)
{
	__u16 sqn = q->sqn & QM_DBELL_SQN_MASK;
	void *base = q->doorbell_base;
	__u64 doorbell;

	doorbell = (__u64)sqn | ((__u64)(cmd & QM_DBELL_CMD_MASK) <<
		   QM_V2_DBELL_CMD_SHIFT);
	doorbell |= ((__u64)idx | ((__u64)priority << QM_DBELL_CMD_SHIFT)) <<
		    QM_DBELL_HLF_SHIFT;
	*((__u64 *)base) = doorbell;

	return 0;
}

static int qm_set_queue_regions(struct wd_queue *q)
{
	struct q_info *qinfo = q->qinfo;
	struct qm_queue_info *info = qinfo->priv;

	info->sq_base = wd_drv_mmap_qfr(q, WD_UACCE_QFRT_DUS, 0);
	if (info->sq_base == MAP_FAILED) {
		info->sq_base = NULL;
		WD_ERR("mmap dus fail\n");
		return -ENOMEM;
	}

	info->mmio_base = wd_drv_mmap_qfr(q, WD_UACCE_QFRT_MMIO, 0);
	if (info->mmio_base == MAP_FAILED) {
		wd_drv_unmmap_qfr(q, info->sq_base, WD_UACCE_QFRT_DUS, 0);
		info->sq_base = NULL;
		info->mmio_base = NULL;
		WD_ERR("mmap mmio fail\n");
		return -ENOMEM;
	}

	return 0;
}

static void qm_unset_queue_regions(struct wd_queue *q)
{
	struct q_info *qinfo = q->qinfo;
	struct qm_queue_info *info = qinfo->priv;

	wd_drv_unmmap_qfr(q, info->mmio_base, WD_UACCE_QFRT_MMIO, 0);
	wd_drv_unmmap_qfr(q, info->sq_base, WD_UACCE_QFRT_DUS, 0);
	info->sq_base = NULL;
	info->mmio_base = NULL;
}

static bool hpre_alg_info_init(struct wd_queue *q, const char *alg)
{
	struct wcrypto_paras *priv = &q->capa.priv;
	struct q_info *qinfo = q->qinfo;
	struct qm_queue_info *info = qinfo->priv;
	bool is_find = true;

	if (!strncmp(alg, "rsa", strlen("rsa"))) {
		qinfo->atype = WCRYPTO_RSA;
		info->sqe_size = QM_HPRE_BD_SIZE;
		info->sqe_fill[WCRYPTO_RSA] = qm_fill_rsa_sqe;
		info->sqe_parse[WCRYPTO_RSA] = qm_parse_rsa_sqe;
	} else if (!strncmp(alg, "dh", strlen("dh"))) {
		qinfo->atype = WCRYPTO_DH;
		info->sqe_size = QM_HPRE_BD_SIZE;
		info->sqe_fill[WCRYPTO_DH] = qm_fill_dh_sqe;
		info->sqe_parse[WCRYPTO_DH] = qm_parse_dh_sqe;
	} else if (!strncmp(alg, "ecdh", strlen("ecdh"))) {
		qinfo->atype = WCRYPTO_ECDH;
		info->sqe_size = QM_HPRE_BD_SIZE;
		info->sqe_fill[WCRYPTO_ECDH] = qm_fill_ecc_sqe;
		info->sqe_parse[WCRYPTO_ECDH] = qm_parse_ecc_sqe;
		priv->direction = 1;
	} else if (!strncmp(alg, "x448", strlen("x448"))) {
		qinfo->atype = WCRYPTO_X448;
		info->sqe_size = QM_HPRE_BD_SIZE;
		info->sqe_fill[WCRYPTO_X448] = qm_fill_ecc_sqe;
		info->sqe_parse[WCRYPTO_X448] = qm_parse_ecc_sqe;
		priv->direction = 1;
	} else if (!strncmp(alg, "x25519", strlen("x25519"))) {
		qinfo->atype = WCRYPTO_X25519;
		info->sqe_size = QM_HPRE_BD_SIZE;
		info->sqe_fill[WCRYPTO_X25519] = qm_fill_ecc_sqe;
		info->sqe_parse[WCRYPTO_X25519] = qm_parse_ecc_sqe;
		priv->direction = 1;
	} else if (!strncmp(alg, "ecdsa", strlen("ecdsa"))) {
		qinfo->atype = WCRYPTO_ECDSA;
		info->sqe_size = QM_HPRE_BD_SIZE;
		info->sqe_fill[WCRYPTO_ECDSA] = qm_fill_ecc_sqe;
		info->sqe_parse[WCRYPTO_ECDSA] = qm_parse_ecc_sqe;
		priv->direction = 1;
	} else if (!strncmp(alg, "sm2", strlen("sm2"))) {
		qinfo->atype = WCRYPTO_SM2;
		info->sqe_size = QM_HPRE_BD_SIZE;
		info->sqe_fill[WCRYPTO_SM2] = qm_fill_ecc_sqe;
		info->sqe_parse[WCRYPTO_SM2] = qm_parse_ecc_sqe;
		priv->direction = 1;
	} else {
		is_find = false;
	}

	return is_find;
}

static bool sec_alg_info_init(struct q_info *qinfo, const char *alg)
{
	struct qm_queue_info *info = qinfo->priv;
	bool is_find = true;

	if (!strncmp(alg, "cipher", strlen("cipher"))) {
		qinfo->atype = WCRYPTO_CIPHER;
		info->sqe_size = QM_SEC_BD_SIZE;
		if (strstr(qinfo->hw_type, HISI_QM_API_VER2_BASE)) {
			info->sqe_fill[WCRYPTO_CIPHER] = qm_fill_cipher_sqe;
			info->sqe_parse[WCRYPTO_CIPHER] = qm_parse_cipher_sqe;
		} else if (strstr(qinfo->hw_type, HISI_QM_API_VER3_BASE)) {
			info->sqe_fill[WCRYPTO_CIPHER] = qm_fill_cipher_bd3_sqe;
			info->sqe_parse[WCRYPTO_CIPHER] = qm_parse_cipher_bd3_sqe;
		}
	} else if (!strncmp(alg, "digest", strlen("digest"))) {
		qinfo->atype = WCRYPTO_DIGEST;
		info->sqe_size = QM_SEC_BD_SIZE;
		if (strstr(qinfo->hw_type, HISI_QM_API_VER2_BASE)) {
			info->sqe_fill[WCRYPTO_DIGEST] = qm_fill_digest_sqe;
			info->sqe_parse[WCRYPTO_DIGEST] = qm_parse_digest_sqe;
		} else if (strstr(qinfo->hw_type, HISI_QM_API_VER3_BASE)) {
			info->sqe_fill[WCRYPTO_DIGEST] = qm_fill_digest_bd3_sqe;
			info->sqe_parse[WCRYPTO_DIGEST] = qm_parse_digest_bd3_sqe;
		}
	} else if (!strncmp(alg, "aead", strlen("aead"))) {
		qinfo->atype = WCRYPTO_AEAD;
		info->sqe_size = QM_SEC_BD_SIZE;
		if (strstr(qinfo->hw_type, HISI_QM_API_VER2_BASE)) {
			info->sqe_fill[WCRYPTO_AEAD] = qm_fill_aead_sqe;
			info->sqe_parse[WCRYPTO_AEAD] = qm_parse_aead_sqe;
		} else if (strstr(qinfo->hw_type, HISI_QM_API_VER3_BASE)) {
			info->sqe_fill[WCRYPTO_AEAD] = qm_fill_aead_bd3_sqe;
			info->sqe_parse[WCRYPTO_AEAD] = qm_parse_aead_bd3_sqe;
		}
	} else {
		is_find = false;
	}

	return is_find;
}

static bool zip_alg_info_init(struct q_info *qinfo, const char *alg)
{
	struct qm_queue_info *info = qinfo->priv;
	bool is_find = false;

	if (!strncmp(alg, "zlib", strlen("zlib")) ||
	    !strncmp(alg, "gzip", strlen("gzip")) ||
	    !strncmp(alg, "deflate", strlen("deflate")) ||
	    !strncmp(alg, "lz77_zstd", strlen("lz77_zstd"))) {
		qinfo->atype = WCRYPTO_COMP;
		info->sqe_size = QM_ZIP_BD_SIZE;
		if (strstr(qinfo->hw_type, HISI_QM_API_VER2_BASE)) {
			info->sqe_fill[WCRYPTO_COMP] = qm_fill_zip_sqe;
			info->sqe_parse[WCRYPTO_COMP] = qm_parse_zip_sqe;
		} else if (strstr(qinfo->hw_type, HISI_QM_API_VER3_BASE)) {
			info->sqe_fill[WCRYPTO_COMP] = qm_fill_zip_sqe_v3;
			info->sqe_parse[WCRYPTO_COMP] = qm_parse_zip_sqe_v3;
		}
		is_find = true;
	}

	return is_find;
}

static int qm_set_queue_alg_info(struct wd_queue *q)
{
	const char *alg = q->capa.alg;
	struct q_info *qinfo = q->qinfo;
	struct qm_queue_info *info = qinfo->priv;
	struct wcrypto_paras *priv = &q->capa.priv;
	int ret = -WD_EINVAL;

	if (hpre_alg_info_init(q, alg)) {
		ret = WD_SUCCESS;
	} else if (zip_alg_info_init(qinfo, alg)) {
		ret = WD_SUCCESS;
	} else if (sec_alg_info_init(qinfo, alg)) {
		ret = WD_SUCCESS;
	} else if (!strncmp(alg, "xts(aes)", strlen("xts(aes)")) ||
		!strncmp(alg, "xts(sm4)", strlen("xts(sm4)"))) {
		qinfo->atype = WCRYPTO_CIPHER;
		if (strstr(q->dev_path, "zip")) {
			info->sqe_size = QM_ZIP_BD_SIZE;
			info->sqe_fill[WCRYPTO_CIPHER] = qm_fill_zip_cipher_sqe;
			info->sqe_parse[WCRYPTO_CIPHER] = qm_parse_zip_cipher_sqe;
			ret = WD_SUCCESS;
		} else if (strstr(q->dev_path, "sec")) {
			priv->direction = 0;
			info->sqe_size = QM_SEC_BD_SIZE;
			info->sqe_fill[WCRYPTO_CIPHER] = qm_fill_cipher_sqe;
			info->sqe_parse[WCRYPTO_CIPHER] = qm_parse_cipher_sqe;
			ret = WD_SUCCESS;
		} else { /* To be extended */
			WD_ERR("queue xts alg engine err!\n");
		}
	} else { /* To be extended */
		WD_ERR("queue alg err!\n");
	}

	return ret;
}

static int qm_set_db_info(struct q_info *qinfo)
{
	struct qm_queue_info *info = qinfo->priv;

	if (strstr(qinfo->hw_type, HISI_QM_API_VER2_BASE) ||
	strstr(qinfo->hw_type, HISI_QM_API_VER3_BASE)) {
		info->db = qm_db_v2;
		info->doorbell_base = info->mmio_base + QM_V2_DOORBELL_OFFSET;
	} else if (strstr(qinfo->hw_type, HISI_QM_API_VER_BASE)) {
		info->db = qm_db_v1;
		info->doorbell_base = info->mmio_base + QM_DOORBELL_OFFSET;
	} else {
		WD_ERR("hw version mismatch!\n");
		return -EINVAL;
	}

	return 0;
}

static int qm_set_queue_info(struct wd_queue *q)
{
	struct wcrypto_paras *priv = &q->capa.priv;
	struct q_info *qinfo = q->qinfo;
	struct qm_queue_info *info = qinfo->priv;
	struct hisi_qp_ctx qp_ctx;
	size_t psize;
	int ret;

	ret = qm_set_queue_regions(q);
	if (ret)
		return -EINVAL;
	if (!info->sqe_size) {
		WD_ERR("sqe size =%d err!\n", info->sqe_size);
		ret = -EINVAL;
		goto err_with_regions;
	}
	info->cq_base = (void *)((uintptr_t)info->sq_base +
			info->sqe_size * QM_Q_DEPTH);

	/* Protect the virtual address of CQ to avoid being over written */
	psize = qinfo->qfrs_offset[WD_UACCE_QFRT_DUS] -
		info->sqe_size * QM_Q_DEPTH;
	ret = mprotect(info->cq_base, psize, PROT_READ);
	if (ret) {
		WD_ERR("cqe mprotect set err!\n");
		ret = -EINVAL;
		goto err_with_regions;
	}

	/* The last 32 bits of DUS show device or qp statuses */
	info->ds_tx_base = info->sq_base + qinfo->qfrs_offset[WD_UACCE_QFRT_DUS] -
		sizeof(uint32_t);
	info->ds_rx_base = info->ds_tx_base - sizeof(uint32_t);
	ret = qm_set_db_info(qinfo);
	if (ret)
		goto err_with_regions;

	info->sq_tail_index = 0;
	info->cq_head_index = 0;
	info->cqc_phase = 1;
	info->used = 0;
	qp_ctx.qc_type = priv->direction;
	qp_ctx.id = 0;
	ret = ioctl(qinfo->fd, WD_UACCE_CMD_QM_SET_QP_CTX, &qp_ctx);
	if (ret < 0) {
		WD_ERR("hisi qm set qc_type fail, use default!\n");
		goto err_with_regions;
	}
	info->sqn = qp_ctx.id;

	return 0;

err_with_regions:
	qm_unset_queue_regions(q);
	return ret;
}

int qm_init_queue(struct wd_queue *q)
{
	struct q_info *qinfo = q->qinfo;
	struct qm_queue_info *info;
	int ret = -ENOMEM;

	info = calloc(1, sizeof(*info));
	if (!info) {
		WD_ERR("no mem!\n");
		return ret;
	}
	info->q = q;
	qinfo->priv = info;
	ret = qm_set_queue_alg_info(q);
	if (ret < 0)
		goto err_with_priv;
	ret = qm_set_queue_info(q);
	if (ret < 0)
		goto err_with_priv;
	info->sqe_fill_priv = NULL;
	info->sqe_parse_priv = NULL;

	return 0;

err_with_priv:
	free(qinfo->priv);
	qinfo->priv = NULL;
	return ret;
}

void qm_uninit_queue(struct wd_queue *q)
{
	struct q_info *qinfo = q->qinfo;

	qm_unset_queue_regions(q);
	free(qinfo->priv);
	qinfo->priv = NULL;
}

static void qm_tx_update(struct qm_queue_info *info, __u16 idx, __u32 num)
{
	info->sq_tail_index = idx;
	info->db(info, DOORBELL_CMD_SQ, idx, 0);
	__atomic_add_fetch(&info->used, num, __ATOMIC_RELAXED);
}

int qm_send(struct wd_queue *q, void **req, __u32 num)
{
	struct q_info *qinfo = q->qinfo;
	struct qm_queue_info *info = qinfo->priv;
	__u16 sq_tail;
	int i, ret;

	if (unlikely(wd_reg_read(info->ds_tx_base) == 1)) {
		WD_ERR("wd queue hw error happened before qm send!\n");
		return -WD_HW_EACCESS;
	}

	wd_spinlock(&info->sd_lock);
	if (unlikely(__atomic_load_n(&info->used, __ATOMIC_RELAXED) >
		     QM_Q_DEPTH - num - 1)) {
		wd_unspinlock(&info->sd_lock);
		WD_ERR("queue is full!\n");
		return -WD_EBUSY;
	}

	sq_tail = info->sq_tail_index;
	for (i = 0; i < num; i++) {
		ret = info->sqe_fill[qinfo->atype](req[i], qinfo->priv,
				sq_tail);
		if (unlikely(ret != WD_SUCCESS)) {
			wd_unspinlock(&info->sd_lock);
			WD_ERR("sqe fill error, ret %d!\n", ret);
			return -WD_EINVAL;
		}

		if (sq_tail == QM_Q_DEPTH - 1)
			sq_tail = 0;
		else
			sq_tail++;
	}

	/* make sure the request is all in memory before doorbell */
	mb();
	qm_tx_update(info, sq_tail, num);
	wd_unspinlock(&info->sd_lock);

	return WD_SUCCESS;
}

static void qm_rx_update(struct qm_queue_info *info, __u16 idx, __u32 num)
{
	info->cq_head_index = idx;
	info->db(info, DOORBELL_CMD_CQ, idx, 0);
	__atomic_sub_fetch(&info->used, num, __ATOMIC_RELAXED);
}

static void qm_rx_from_cache(struct qm_queue_info *info, void **resp, __u32 num)
{
	__u16 idx = info->cq_head_index;
	int i;

	for (i = 0; i < num; i++) {
		resp[i] = info->req_cache[idx];
		info->req_cache[idx] = NULL;

		if (idx == QM_Q_DEPTH - 1) {
			info->cqc_phase = !(info->cqc_phase);
			idx = 0;
		} else {
			idx++;
		}
	}

	info->cq_head_index = idx;
	__atomic_sub_fetch(&info->used, num, __ATOMIC_RELAXED);
}

int qm_recv(struct wd_queue *q, void **resp, __u32 num)
{
	struct q_info *qinfo = q->qinfo;
	struct qm_queue_info *info = qinfo->priv;
	__u16 cq_head, sq_head;
	struct cqe *cqe;
	int i, ret;
	void *sqe;

	if (unlikely(wd_reg_read(info->ds_rx_base) == 1)) {
		wd_spinlock(&info->rc_lock);
		qm_rx_from_cache(info, resp, num);
		wd_unspinlock(&info->rc_lock);
		return -WD_HW_EACCESS;
	}

	wd_spinlock(&info->rc_lock);
	cq_head = info->cq_head_index;
	for (i = 0; i < num; i++) {
		cqe = info->cq_base + cq_head * sizeof(struct cqe);
		if (info->cqc_phase != CQE_PHASE(cqe))
			break;

		mb(); /* make sure the data is all in memory before read */
		sq_head = CQE_SQ_HEAD_INDEX(cqe);
		if (unlikely(sq_head >= QM_Q_DEPTH)) {
			wd_unspinlock(&info->rc_lock);
			WD_ERR("CQE_SQ_HEAD_INDEX(%u) error\n", sq_head);
			return -WD_EIO;
		}

		sqe = (void *)((uintptr_t)info->sq_base + sq_head * info->sqe_size);
		ret = info->sqe_parse[qinfo->atype](sqe,
				(const struct qm_queue_info *)info,
				cq_head, (__u16)(uintptr_t)resp[i]);
		if (!ret) {
			break;
		} else if (ret < 0) {
			wd_unspinlock(&info->rc_lock);
			WD_ERR("recv sqe error %u\n", sq_head);
			return ret;
		}

		resp[i] = info->req_cache[cq_head];
		info->req_cache[cq_head] = NULL;
		if (cq_head == QM_Q_DEPTH - 1) {
			info->cqc_phase = !(info->cqc_phase);
			cq_head = 0;
		} else {
			cq_head++;
		}
	}

	if (i)
		qm_rx_update(info, cq_head, i);

	wd_unspinlock(&info->rc_lock);
	if (wd_reg_read(info->ds_rx_base) == 1) {
		WD_ERR("wd queue hw error happened in qm receive!\n");
		return -WD_HW_EACCESS;
	}

	return i;
}

static int hw_type_check(struct wd_queue *q, const char *hw_type)
{
	const char *drv = wd_get_drv(q);

	if (!hw_type || !drv)
		return 1;

	return strcmp(drv, hw_type);
}

int hisi_qm_inject_op_register(struct wd_queue *q, struct hisi_qm_inject_op *op)
{
	struct q_info *qinfo = q->qinfo;
	struct qm_queue_info *info = qinfo->priv;

	if (!op || !op->sqe_fill_priv || !op->sqe_parse_priv) {
		WD_ERR("inject option is invalid!\n");
		return -WD_EINVAL;
	}

	if (hw_type_check(q, op->hw_type)) {
		WD_ERR("inject option hw compare error!\n");
		return -WD_EINVAL;
	}

	info->sqe_fill_priv = op->sqe_fill_priv;
	info->sqe_parse_priv = op->sqe_parse_priv;

	return 0;
}
