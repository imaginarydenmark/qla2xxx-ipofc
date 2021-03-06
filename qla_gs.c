/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2014 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#include "qla_def.h"
#include "qla_target.h"
#include <linux/utsname.h>

static int qla2x00_sns_ga_nxt(scsi_qla_host_t *, fc_port_t *);
static int qla2x00_sns_gid_pt(scsi_qla_host_t *, sw_info_t *);
static int qla2x00_sns_gpn_id(scsi_qla_host_t *, sw_info_t *);
static int qla2x00_sns_gnn_id(scsi_qla_host_t *, sw_info_t *);
static int qla2x00_sns_rft_id(scsi_qla_host_t *);
static int qla2x00_sns_rnn_id(scsi_qla_host_t *);

/**
 * qla2x00_prep_ms_iocb() - Prepare common MS/CT IOCB fields for SNS CT query.
 * @ha: HA context
 * @req_size: request size in bytes
 * @rsp_size: response size in bytes
 *
 * Returns a pointer to the @ha's ms_iocb.
 */
void *
qla2x00_prep_ms_iocb(scsi_qla_host_t *vha, uint32_t req_size, uint32_t rsp_size)
{
	struct qla_hw_data *ha = vha->hw;
	ms_iocb_entry_t *ms_pkt;

	ms_pkt = ha->ms_iocb;
	memset(ms_pkt, 0, sizeof(ms_iocb_entry_t));

	ms_pkt->entry_type = MS_IOCB_TYPE;
	ms_pkt->entry_count = 1;
	SET_TARGET_ID(ha, ms_pkt->loop_id, SIMPLE_NAME_SERVER);
	ms_pkt->control_flags = __constant_cpu_to_le16(CF_READ | CF_HEAD_TAG);
	ms_pkt->timeout = cpu_to_le16(ha->r_a_tov / 10 * 2);
	ms_pkt->cmd_dsd_count = __constant_cpu_to_le16(1);
	ms_pkt->total_dsd_count = __constant_cpu_to_le16(2);
	ms_pkt->rsp_bytecount = cpu_to_le32(rsp_size);
	ms_pkt->req_bytecount = cpu_to_le32(req_size);

	ms_pkt->dseg_req_address[0] = cpu_to_le32(LSD(ha->ct_sns_dma));
	ms_pkt->dseg_req_address[1] = cpu_to_le32(MSD(ha->ct_sns_dma));
	ms_pkt->dseg_req_length = ms_pkt->req_bytecount;

	ms_pkt->dseg_rsp_address[0] = cpu_to_le32(LSD(ha->ct_sns_dma));
	ms_pkt->dseg_rsp_address[1] = cpu_to_le32(MSD(ha->ct_sns_dma));
	ms_pkt->dseg_rsp_length = ms_pkt->rsp_bytecount;

	vha->qla_stats.control_requests++;

	return (ms_pkt);
}

/**
 * qla24xx_prep_ms_iocb() - Prepare common CT IOCB fields for SNS CT query.
 * @ha: HA context
 * @req_size: request size in bytes
 * @rsp_size: response size in bytes
 *
 * Returns a pointer to the @ha's ms_iocb.
 */
void *
qla24xx_prep_ms_iocb(scsi_qla_host_t *vha, uint32_t req_size, uint32_t rsp_size)
{
	struct qla_hw_data *ha = vha->hw;
	struct ct_entry_24xx *ct_pkt;

	ct_pkt = (struct ct_entry_24xx *)ha->ms_iocb;
	memset(ct_pkt, 0, sizeof(struct ct_entry_24xx));

	ct_pkt->entry_type = CT_IOCB_TYPE;
	ct_pkt->entry_count = 1;
	ct_pkt->nport_handle = __constant_cpu_to_le16(NPH_SNS);
	ct_pkt->timeout = cpu_to_le16(ha->r_a_tov / 10 * 2);
	ct_pkt->cmd_dsd_count = __constant_cpu_to_le16(1);
	ct_pkt->rsp_dsd_count = __constant_cpu_to_le16(1);
	ct_pkt->rsp_byte_count = cpu_to_le32(rsp_size);
	ct_pkt->cmd_byte_count = cpu_to_le32(req_size);

	ct_pkt->dseg_0_address[0] = cpu_to_le32(LSD(ha->ct_sns_dma));
	ct_pkt->dseg_0_address[1] = cpu_to_le32(MSD(ha->ct_sns_dma));
	ct_pkt->dseg_0_len = ct_pkt->cmd_byte_count;

	ct_pkt->dseg_1_address[0] = cpu_to_le32(LSD(ha->ct_sns_dma));
	ct_pkt->dseg_1_address[1] = cpu_to_le32(MSD(ha->ct_sns_dma));
	ct_pkt->dseg_1_len = ct_pkt->rsp_byte_count;
	ct_pkt->vp_index = vha->vp_idx;

	vha->qla_stats.control_requests++;

	return (ct_pkt);
}

/**
 * qla2x00_prep_ct_req() - Prepare common CT request fields for SNS query.
 * @ct_req: CT request buffer
 * @cmd: GS command
 * @rsp_size: response size in bytes
 *
 * Returns a pointer to the intitialized @ct_req.
 */
static inline struct ct_sns_req *
qla2x00_prep_ct_req(struct ct_sns_pkt *p, uint16_t cmd, uint16_t rsp_size)
{
	memset(p, 0, sizeof(struct ct_sns_pkt));

	p->p.req.header.revision = 0x01;
	p->p.req.header.gs_type = 0xFC;
	p->p.req.header.gs_subtype = 0x02;
	p->p.req.command = cpu_to_be16(cmd);
	p->p.req.max_rsp_size = cpu_to_be16((rsp_size - 16) / 4);

	return &p->p.req;
}

static int
qla2x00_chk_ms_status(scsi_qla_host_t *vha, ms_iocb_entry_t *ms_pkt,
    struct ct_sns_rsp *ct_rsp, const char *routine)
{
	int rval;
	uint16_t comp_status;
	struct qla_hw_data *ha = vha->hw;

	rval = QLA_FUNCTION_FAILED;
	if (ms_pkt->entry_status != 0) {
		ql_dbg(ql_dbg_disc, vha, 0x2031,
		    "%s failed, error status (%x) on port_id: %06x.\n",
		    routine, ms_pkt->entry_status, vha->d_id.b24);
	} else {
		if (IS_FWI2_CAPABLE(ha))
			comp_status = le16_to_cpu(
			    ((struct ct_entry_24xx *)ms_pkt)->comp_status);
		else
			comp_status = le16_to_cpu(ms_pkt->status);
		switch (comp_status) {
		case CS_COMPLETE:
		case CS_DATA_UNDERRUN:
		case CS_DATA_OVERRUN:		/* Overrun? */
			if (ct_rsp->header.response !=
			    __constant_cpu_to_be16(CT_ACCEPT_RESPONSE)) {
				ql_dbg(ql_dbg_disc + ql_dbg_buffer, vha, 0x2077,
				    "%s failed rejected request on port_id: "
				    "%06x Completion status 0x%x, "
				    "response 0x%x\n", routine,
				    vha->d_id.b24, comp_status,
				    ct_rsp->header.response);
				ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, vha,
				    0x2078, (uint8_t *)&ct_rsp->header,
				    sizeof(struct ct_rsp_hdr));
				rval = QLA_INVALID_COMMAND;
			} else
				rval = QLA_SUCCESS;
			break;
		default:
			ql_dbg(ql_dbg_disc, vha, 0x2033,
			    "%s failed, completion status (%x) on port_id: "
			    "%06x.\n", routine, comp_status, vha->d_id.b24);
			break;
		}
	}
	return rval;
}

/**
 * qla2x00_ga_nxt() - SNS scan for fabric devices via GA_NXT command.
 * @ha: HA context
 * @fcport: fcport entry to updated
 *
 * Returns 0 on success.
 */
int
qla2x00_ga_nxt(scsi_qla_host_t *vha, fc_port_t *fcport)
{
	int		rval;

	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;
	struct qla_hw_data *ha = vha->hw;

	if (IS_QLA2100(ha) || IS_QLA2200(ha))
		return qla2x00_sns_ga_nxt(vha, fcport);

	/* Issue GA_NXT */
	/* Prepare common MS IOCB */
	ms_pkt = ha->isp_ops->prep_ms_iocb(vha, GA_NXT_REQ_SIZE,
	    GA_NXT_RSP_SIZE);

	/* Prepare CT request */
	ct_req = qla2x00_prep_ct_req(ha->ct_sns, GA_NXT_CMD,
	    GA_NXT_RSP_SIZE);
	ct_rsp = &ha->ct_sns->p.rsp;

	/* Prepare CT arguments -- port_id */
	ct_req->req.port_id.port_id[0] = fcport->d_id.b.domain;
	ct_req->req.port_id.port_id[1] = fcport->d_id.b.area;
	ct_req->req.port_id.port_id[2] = fcport->d_id.b.al_pa;

	/* Execute MS IOCB */
	rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
	    sizeof(ms_iocb_entry_t));
	if (rval != QLA_SUCCESS) {
		/*EMPTY*/
		ql_dbg(ql_dbg_disc, vha, 0x2062,
		    "GA_NXT issue IOCB failed (%d).\n", rval);
	} else if (qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp, "GA_NXT") !=
	    QLA_SUCCESS) {
		rval = QLA_FUNCTION_FAILED;
	} else {
		/* Populate fc_port_t entry. */
		fcport->d_id.b.domain = ct_rsp->rsp.ga_nxt.port_id[0];
		fcport->d_id.b.area = ct_rsp->rsp.ga_nxt.port_id[1];
		fcport->d_id.b.al_pa = ct_rsp->rsp.ga_nxt.port_id[2];

		memcpy(fcport->node_name, ct_rsp->rsp.ga_nxt.node_name,
		    WWN_SIZE);
		memcpy(fcport->port_name, ct_rsp->rsp.ga_nxt.port_name,
		    WWN_SIZE);

		fcport->fc4_type = (ct_rsp->rsp.ga_nxt.fc4_types[2] & BIT_0) ?
		    FC4_TYPE_FCP_SCSI : FC4_TYPE_OTHER;

		if (ct_rsp->rsp.ga_nxt.port_type != NS_N_PORT_TYPE &&
		    ct_rsp->rsp.ga_nxt.port_type != NS_NL_PORT_TYPE)
			fcport->d_id.b.domain = 0xf0;

		ql_dbg(ql_dbg_disc, vha, 0x2063,
		    "GA_NXT entry - nn %8phN pn %8phN port_id=%06x.\n",
		    fcport->node_name, fcport->port_name,
		    fcport->d_id.b24);
	}

	return (rval);
}

static inline int
qla2x00_gid_pt_rsp_size(scsi_qla_host_t *vha)
{
	return vha->hw->max_fibre_devices * 4 + 16;
}

/**
 * qla2x00_gid_pt() - SNS scan for fabric devices via GID_PT command.
 * @ha: HA context
 * @list: switch info entries to populate
 *
 * NOTE: Non-Nx_Ports are not requested.
 *
 * Returns 0 on success.
 */
int
qla2x00_gid_pt(scsi_qla_host_t *vha, sw_info_t *list)
{
	int		rval;
	uint16_t	i;

	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;

	struct ct_sns_gid_pt_data *gid_data;
	struct qla_hw_data *ha = vha->hw;
	uint16_t gid_pt_rsp_size;

	if (IS_QLA2100(ha) || IS_QLA2200(ha))
		return qla2x00_sns_gid_pt(vha, list);

	gid_data = NULL;
	gid_pt_rsp_size = qla2x00_gid_pt_rsp_size(vha);
	/* Issue GID_PT */
	/* Prepare common MS IOCB */
	ms_pkt = ha->isp_ops->prep_ms_iocb(vha, GID_PT_REQ_SIZE,
	    gid_pt_rsp_size);

	/* Prepare CT request */
	ct_req = qla2x00_prep_ct_req(ha->ct_sns, GID_PT_CMD, gid_pt_rsp_size);
	ct_rsp = &ha->ct_sns->p.rsp;

	/* Prepare CT arguments -- port_type */
	ct_req->req.gid_pt.port_type = NS_NX_PORT_TYPE;

	/* Execute MS IOCB */
	rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
	    sizeof(ms_iocb_entry_t));
	if (rval != QLA_SUCCESS) {
		/*EMPTY*/
		ql_dbg(ql_dbg_disc, vha, 0x2055,
		    "GID_PT issue IOCB failed (%d).\n", rval);
	} else if (qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp, "GID_PT") !=
	    QLA_SUCCESS) {
		rval = QLA_FUNCTION_FAILED;
	} else {
		/* Set port IDs in switch info list. */
		for (i = 0; i < ha->max_fibre_devices; i++) {
			gid_data = &ct_rsp->rsp.gid_pt.entries[i];
			list[i].d_id.b.domain = gid_data->port_id[0];
			list[i].d_id.b.area = gid_data->port_id[1];
			list[i].d_id.b.al_pa = gid_data->port_id[2];
			memset(list[i].fabric_port_name, 0, WWN_SIZE);
			list[i].fp_speed = PORT_SPEED_UNKNOWN;

			/* Last one exit. */
			if (gid_data->control_byte & BIT_7) {
				list[i].d_id.b.rsvd_1 = gid_data->control_byte;
				break;
			}
		}

		/*
		 * If we've used all available slots, then the switch is
		 * reporting back more devices than we can handle with this
		 * single call.  Return a failed status, and let GA_NXT handle
		 * the overload.
		 */
		if (i == ha->max_fibre_devices)
			rval = QLA_FUNCTION_FAILED;
	}

	return (rval);
}

/**
 * qla2x00_gpn_id() - SNS Get Port Name (GPN_ID) query.
 * @ha: HA context
 * @list: switch info entries to populate
 *
 * Returns 0 on success.
 */
int
qla2x00_gpn_id(scsi_qla_host_t *vha, sw_info_t *list)
{
	int		rval = QLA_SUCCESS;
	uint16_t	i;

	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;
	struct qla_hw_data *ha = vha->hw;

	if (IS_QLA2100(ha) || IS_QLA2200(ha))
		return qla2x00_sns_gpn_id(vha, list);

	for (i = 0; i < ha->max_fibre_devices; i++) {
		/* Issue GPN_ID */
		/* Prepare common MS IOCB */
		ms_pkt = ha->isp_ops->prep_ms_iocb(vha, GPN_ID_REQ_SIZE,
		    GPN_ID_RSP_SIZE);

		/* Prepare CT request */
		ct_req = qla2x00_prep_ct_req(ha->ct_sns, GPN_ID_CMD,
		    GPN_ID_RSP_SIZE);
		ct_rsp = &ha->ct_sns->p.rsp;

		/* Prepare CT arguments -- port_id */
		ct_req->req.port_id.port_id[0] = list[i].d_id.b.domain;
		ct_req->req.port_id.port_id[1] = list[i].d_id.b.area;
		ct_req->req.port_id.port_id[2] = list[i].d_id.b.al_pa;

		/* Execute MS IOCB */
		rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
		    sizeof(ms_iocb_entry_t));
		if (rval != QLA_SUCCESS) {
			/*EMPTY*/
			ql_dbg(ql_dbg_disc, vha, 0x2056,
			    "GPN_ID issue IOCB failed (%d).\n", rval);
			break;
		} else if (qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp,
		    "GPN_ID") != QLA_SUCCESS) {
			rval = QLA_FUNCTION_FAILED;
			break;
		} else {
			/* Save portname */
			memcpy(list[i].port_name,
			    ct_rsp->rsp.gpn_id.port_name, WWN_SIZE);
		}

		/* Last device exit. */
		if (list[i].d_id.b.rsvd_1 != 0)
			break;
	}

	return (rval);
}

/**
 * qla2x00_gnn_id() - SNS Get Node Name (GNN_ID) query.
 * @ha: HA context
 * @list: switch info entries to populate
 *
 * Returns 0 on success.
 */
int
qla2x00_gnn_id(scsi_qla_host_t *vha, sw_info_t *list)
{
	int		rval = QLA_SUCCESS;
	uint16_t	i;
	struct qla_hw_data *ha = vha->hw;
	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;

	if (IS_QLA2100(ha) || IS_QLA2200(ha))
		return qla2x00_sns_gnn_id(vha, list);

	for (i = 0; i < ha->max_fibre_devices; i++) {
		/* Issue GNN_ID */
		/* Prepare common MS IOCB */
		ms_pkt = ha->isp_ops->prep_ms_iocb(vha, GNN_ID_REQ_SIZE,
		    GNN_ID_RSP_SIZE);

		/* Prepare CT request */
		ct_req = qla2x00_prep_ct_req(ha->ct_sns, GNN_ID_CMD,
		    GNN_ID_RSP_SIZE);
		ct_rsp = &ha->ct_sns->p.rsp;

		/* Prepare CT arguments -- port_id */
		ct_req->req.port_id.port_id[0] = list[i].d_id.b.domain;
		ct_req->req.port_id.port_id[1] = list[i].d_id.b.area;
		ct_req->req.port_id.port_id[2] = list[i].d_id.b.al_pa;

		/* Execute MS IOCB */
		rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
		    sizeof(ms_iocb_entry_t));
		if (rval != QLA_SUCCESS) {
			/*EMPTY*/
			ql_dbg(ql_dbg_disc, vha, 0x2057,
			    "GNN_ID issue IOCB failed (%d).\n", rval);
			break;
		} else if (qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp,
		    "GNN_ID") != QLA_SUCCESS) {
			rval = QLA_FUNCTION_FAILED;
			break;
		} else {
			/* Save nodename */
			memcpy(list[i].node_name,
			    ct_rsp->rsp.gnn_id.node_name, WWN_SIZE);

			ql_dbg(ql_dbg_disc, vha, 0x2058,
			    "GID_PT entry - nn %8phN pn %8phN portid=%06x.\n",
			    list[i].node_name, list[i].port_name,
			    list[i].d_id.b24);
		}

		/* Last device exit. */
		if (list[i].d_id.b.rsvd_1 != 0)
			break;
	}

	return (rval);
}

/**
 * qla2x00_rft_id() - SNS Register FC-4 TYPEs (RFT_ID) supported by the HBA.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
int
qla2x00_rft_id(scsi_qla_host_t *vha)
{
	int		rval;
	struct qla_hw_data *ha = vha->hw;
	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;

	if (IS_QLA2100(ha) || IS_QLA2200(ha))
		return qla2x00_sns_rft_id(vha);

	/* Issue RFT_ID */
	/* Prepare common MS IOCB */
	ms_pkt = ha->isp_ops->prep_ms_iocb(vha, RFT_ID_REQ_SIZE,
	    RFT_ID_RSP_SIZE);

	/* Prepare CT request */
	ct_req = qla2x00_prep_ct_req(ha->ct_sns, RFT_ID_CMD,
	    RFT_ID_RSP_SIZE);
	ct_rsp = &ha->ct_sns->p.rsp;

	/* Prepare CT arguments -- port_id, FC-4 types */
	ct_req->req.rft_id.port_id[0] = vha->d_id.b.domain;
	ct_req->req.rft_id.port_id[1] = vha->d_id.b.area;
	ct_req->req.rft_id.port_id[2] = vha->d_id.b.al_pa;

	ct_req->req.rft_id.fc4_types[2] = 0x01;		/* FCP-3 */

	/* Execute MS IOCB */
	rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
	    sizeof(ms_iocb_entry_t));
	if (rval != QLA_SUCCESS) {
		/*EMPTY*/
		ql_dbg(ql_dbg_disc, vha, 0x2043,
		    "RFT_ID issue IOCB failed (%d).\n", rval);
	} else if (qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp, "RFT_ID") !=
	    QLA_SUCCESS) {
		rval = QLA_FUNCTION_FAILED;
	} else {
		ql_dbg(ql_dbg_disc, vha, 0x2044,
		    "RFT_ID exiting normally.\n");
	}

	return (rval);
}

/**
 * qla2x00_rff_id() - SNS Register FC-4 Features (RFF_ID) supported by the HBA.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
int
qla2x00_rff_id(scsi_qla_host_t *vha)
{
	int		rval;
	struct qla_hw_data *ha = vha->hw;
	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;

	if (IS_QLA2100(ha) || IS_QLA2200(ha)) {
		ql_dbg(ql_dbg_disc, vha, 0x2046,
		    "RFF_ID call not supported on ISP2100/ISP2200.\n");
		return (QLA_SUCCESS);
	}

	/* Issue RFF_ID */
	/* Prepare common MS IOCB */
	ms_pkt = ha->isp_ops->prep_ms_iocb(vha, RFF_ID_REQ_SIZE,
	    RFF_ID_RSP_SIZE);

	/* Prepare CT request */
	ct_req = qla2x00_prep_ct_req(ha->ct_sns, RFF_ID_CMD,
	    RFF_ID_RSP_SIZE);
	ct_rsp = &ha->ct_sns->p.rsp;

	/* Prepare CT arguments -- port_id, FC-4 feature, FC-4 type */
	ct_req->req.rff_id.port_id[0] = vha->d_id.b.domain;
	ct_req->req.rff_id.port_id[1] = vha->d_id.b.area;
	ct_req->req.rff_id.port_id[2] = vha->d_id.b.al_pa;

	qlt_rff_id(vha, ct_req);

	ct_req->req.rff_id.fc4_type = 0x08;		/* SCSI - FCP */

	/* Execute MS IOCB */
	rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
	    sizeof(ms_iocb_entry_t));
	if (rval != QLA_SUCCESS) {
		/*EMPTY*/
		ql_dbg(ql_dbg_disc, vha, 0x2047,
		    "RFF_ID issue IOCB failed (%d).\n", rval);
	} else if (qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp, "RFF_ID") !=
	    QLA_SUCCESS) {
		rval = QLA_FUNCTION_FAILED;
	} else {
		ql_dbg(ql_dbg_disc, vha, 0x2048,
		    "RFF_ID exiting normally.\n");
	}

	return (rval);
}

/**
 * qla2x00_rnn_id() - SNS Register Node Name (RNN_ID) of the HBA.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
int
qla2x00_rnn_id(scsi_qla_host_t *vha)
{
	int		rval;
	struct qla_hw_data *ha = vha->hw;
	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;

	if (IS_QLA2100(ha) || IS_QLA2200(ha))
		return qla2x00_sns_rnn_id(vha);

	/* Issue RNN_ID */
	/* Prepare common MS IOCB */
	ms_pkt = ha->isp_ops->prep_ms_iocb(vha, RNN_ID_REQ_SIZE,
	    RNN_ID_RSP_SIZE);

	/* Prepare CT request */
	ct_req = qla2x00_prep_ct_req(ha->ct_sns, RNN_ID_CMD, RNN_ID_RSP_SIZE);
	ct_rsp = &ha->ct_sns->p.rsp;

	/* Prepare CT arguments -- port_id, node_name */
	ct_req->req.rnn_id.port_id[0] = vha->d_id.b.domain;
	ct_req->req.rnn_id.port_id[1] = vha->d_id.b.area;
	ct_req->req.rnn_id.port_id[2] = vha->d_id.b.al_pa;

	memcpy(ct_req->req.rnn_id.node_name, vha->node_name, WWN_SIZE);

	/* Execute MS IOCB */
	rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
	    sizeof(ms_iocb_entry_t));
	if (rval != QLA_SUCCESS) {
		/*EMPTY*/
		ql_dbg(ql_dbg_disc, vha, 0x204d,
		    "RNN_ID issue IOCB failed (%d).\n", rval);
	} else if (qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp, "RNN_ID") !=
	    QLA_SUCCESS) {
		rval = QLA_FUNCTION_FAILED;
	} else {
		ql_dbg(ql_dbg_disc, vha, 0x204e,
		    "RNN_ID exiting normally.\n");
	}

	return (rval);
}

size_t
qla2x00_get_sym_node_name(scsi_qla_host_t *vha, uint8_t *snn, size_t size)
{
	struct qla_hw_data *ha = vha->hw;

	if (IS_QLAFX00(ha))
		return scnprintf(snn, size,
		    "%s FW:v%s DVR:v%s", ha->model_number,
		    ha->mr.fw_version, qla2x00_version_str);

	return scnprintf(snn, size,
	    "%s FW:v%d.%02d.%02d DVR:v%s", ha->model_number,
	    ha->fw_major_version, ha->fw_minor_version,
	    ha->fw_subminor_version, qla2x00_version_str);
}

/**
 * qla2x00_rsnn_nn() - SNS Register Symbolic Node Name (RSNN_NN) of the HBA.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
int
qla2x00_rsnn_nn(scsi_qla_host_t *vha)
{
	int		rval;
	struct qla_hw_data *ha = vha->hw;
	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;

	if (IS_QLA2100(ha) || IS_QLA2200(ha)) {
		ql_dbg(ql_dbg_disc, vha, 0x2050,
		    "RSNN_ID call unsupported on ISP2100/ISP2200.\n");
		return (QLA_SUCCESS);
	}

	/* Issue RSNN_NN */
	/* Prepare common MS IOCB */
	/*   Request size adjusted after CT preparation */
	ms_pkt = ha->isp_ops->prep_ms_iocb(vha, 0, RSNN_NN_RSP_SIZE);

	/* Prepare CT request */
	ct_req = qla2x00_prep_ct_req(ha->ct_sns, RSNN_NN_CMD,
	    RSNN_NN_RSP_SIZE);
	ct_rsp = &ha->ct_sns->p.rsp;

	/* Prepare CT arguments -- node_name, symbolic node_name, size */
	memcpy(ct_req->req.rsnn_nn.node_name, vha->node_name, WWN_SIZE);

	/* Prepare the Symbolic Node Name */
	qla2x00_get_sym_node_name(vha, ct_req->req.rsnn_nn.sym_node_name,
	    sizeof(ct_req->req.rsnn_nn.sym_node_name));

	/* Calculate SNN length */
	ct_req->req.rsnn_nn.name_len =
	    (uint8_t)strlen(ct_req->req.rsnn_nn.sym_node_name);

	/* Update MS IOCB request */
	ms_pkt->req_bytecount =
	    cpu_to_le32(24 + 1 + ct_req->req.rsnn_nn.name_len);
	ms_pkt->dseg_req_length = ms_pkt->req_bytecount;

	/* Execute MS IOCB */
	rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
	    sizeof(ms_iocb_entry_t));
	if (rval != QLA_SUCCESS) {
		/*EMPTY*/
		ql_dbg(ql_dbg_disc, vha, 0x2051,
		    "RSNN_NN issue IOCB failed (%d).\n", rval);
	} else if (qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp, "RSNN_NN") !=
	    QLA_SUCCESS) {
		rval = QLA_FUNCTION_FAILED;
	} else {
		ql_dbg(ql_dbg_disc, vha, 0x2052,
		    "RSNN_NN exiting normally.\n");
	}

	return (rval);
}

/**
 * qla2x00_prep_sns_cmd() - Prepare common SNS command request fields for query.
 * @ha: HA context
 * @cmd: GS command
 * @scmd_len: Subcommand length
 * @data_size: response size in bytes
 *
 * Returns a pointer to the @ha's sns_cmd.
 */
static inline struct sns_cmd_pkt *
qla2x00_prep_sns_cmd(scsi_qla_host_t *vha, uint16_t cmd, uint16_t scmd_len,
    uint16_t data_size)
{
	uint16_t		wc;
	struct sns_cmd_pkt	*sns_cmd;
	struct qla_hw_data *ha = vha->hw;

	sns_cmd = ha->sns_cmd;
	memset(sns_cmd, 0, sizeof(struct sns_cmd_pkt));
	wc = data_size / 2;			/* Size in 16bit words. */
	sns_cmd->p.cmd.buffer_length = cpu_to_le16(wc);
	sns_cmd->p.cmd.buffer_address[0] = cpu_to_le32(LSD(ha->sns_cmd_dma));
	sns_cmd->p.cmd.buffer_address[1] = cpu_to_le32(MSD(ha->sns_cmd_dma));
	sns_cmd->p.cmd.subcommand_length = cpu_to_le16(scmd_len);
	sns_cmd->p.cmd.subcommand = cpu_to_le16(cmd);
	wc = (data_size - 16) / 4;		/* Size in 32bit words. */
	sns_cmd->p.cmd.size = cpu_to_le16(wc);

	vha->qla_stats.control_requests++;

	return (sns_cmd);
}

/**
 * qla2x00_sns_ga_nxt() - SNS scan for fabric devices via GA_NXT command.
 * @ha: HA context
 * @fcport: fcport entry to updated
 *
 * This command uses the old Exectute SNS Command mailbox routine.
 *
 * Returns 0 on success.
 */
static int
qla2x00_sns_ga_nxt(scsi_qla_host_t *vha, fc_port_t *fcport)
{
	int		rval = QLA_SUCCESS;
	struct qla_hw_data *ha = vha->hw;
	struct sns_cmd_pkt	*sns_cmd;

	/* Issue GA_NXT. */
	/* Prepare SNS command request. */
	sns_cmd = qla2x00_prep_sns_cmd(vha, GA_NXT_CMD, GA_NXT_SNS_SCMD_LEN,
	    GA_NXT_SNS_DATA_SIZE);

	/* Prepare SNS command arguments -- port_id. */
	sns_cmd->p.cmd.param[0] = fcport->d_id.b.al_pa;
	sns_cmd->p.cmd.param[1] = fcport->d_id.b.area;
	sns_cmd->p.cmd.param[2] = fcport->d_id.b.domain;

	/* Execute SNS command. */
	rval = qla2x00_send_sns(vha, ha->sns_cmd_dma, GA_NXT_SNS_CMD_SIZE / 2,
	    sizeof(struct sns_cmd_pkt));
	if (rval != QLA_SUCCESS) {
		/*EMPTY*/
		ql_dbg(ql_dbg_disc, vha, 0x205f,
		    "GA_NXT Send SNS failed (%d).\n", rval);
	} else if (sns_cmd->p.gan_data[8] != 0x80 ||
	    sns_cmd->p.gan_data[9] != 0x02) {
		ql_dbg(ql_dbg_disc + ql_dbg_buffer, vha, 0x2084,
		    "GA_NXT failed, rejected request ga_nxt_rsp:\n");
		ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, vha, 0x2074,
		    sns_cmd->p.gan_data, 16);
		rval = QLA_FUNCTION_FAILED;
	} else {
		/* Populate fc_port_t entry. */
		fcport->d_id.b.domain = sns_cmd->p.gan_data[17];
		fcport->d_id.b.area = sns_cmd->p.gan_data[18];
		fcport->d_id.b.al_pa = sns_cmd->p.gan_data[19];

		memcpy(fcport->node_name, &sns_cmd->p.gan_data[284], WWN_SIZE);
		memcpy(fcport->port_name, &sns_cmd->p.gan_data[20], WWN_SIZE);

		if (sns_cmd->p.gan_data[16] != NS_N_PORT_TYPE &&
		    sns_cmd->p.gan_data[16] != NS_NL_PORT_TYPE)
			fcport->d_id.b.domain = 0xf0;

		ql_dbg(ql_dbg_disc, vha, 0x2061,
		    "GA_NXT entry - nn %8phN pn %8phN port_id=%06x.\n",
		    fcport->node_name, fcport->port_name,
		    fcport->d_id.b24);
	}

	return (rval);
}

/**
 * qla2x00_sns_gid_pt() - SNS scan for fabric devices via GID_PT command.
 * @ha: HA context
 * @list: switch info entries to populate
 *
 * This command uses the old Exectute SNS Command mailbox routine.
 *
 * NOTE: Non-Nx_Ports are not requested.
 *
 * Returns 0 on success.
 */
static int
qla2x00_sns_gid_pt(scsi_qla_host_t *vha, sw_info_t *list)
{
	int		rval;
	struct qla_hw_data *ha = vha->hw;
	uint16_t	i;
	uint8_t		*entry;
	struct sns_cmd_pkt	*sns_cmd;
	uint16_t gid_pt_sns_data_size;

	gid_pt_sns_data_size = qla2x00_gid_pt_rsp_size(vha);

	/* Issue GID_PT. */
	/* Prepare SNS command request. */
	sns_cmd = qla2x00_prep_sns_cmd(vha, GID_PT_CMD, GID_PT_SNS_SCMD_LEN,
	    gid_pt_sns_data_size);

	/* Prepare SNS command arguments -- port_type. */
	sns_cmd->p.cmd.param[0] = NS_NX_PORT_TYPE;

	/* Execute SNS command. */
	rval = qla2x00_send_sns(vha, ha->sns_cmd_dma, GID_PT_SNS_CMD_SIZE / 2,
	    sizeof(struct sns_cmd_pkt));
	if (rval != QLA_SUCCESS) {
		/*EMPTY*/
		ql_dbg(ql_dbg_disc, vha, 0x206d,
		    "GID_PT Send SNS failed (%d).\n", rval);
	} else if (sns_cmd->p.gid_data[8] != 0x80 ||
	    sns_cmd->p.gid_data[9] != 0x02) {
		ql_dbg(ql_dbg_disc, vha, 0x202f,
		    "GID_PT failed, rejected request, gid_rsp:\n");
		ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, vha, 0x2081,
		    sns_cmd->p.gid_data, 16);
		rval = QLA_FUNCTION_FAILED;
	} else {
		/* Set port IDs in switch info list. */
		for (i = 0; i < ha->max_fibre_devices; i++) {
			entry = &sns_cmd->p.gid_data[(i * 4) + 16];
			list[i].d_id.b.domain = entry[1];
			list[i].d_id.b.area = entry[2];
			list[i].d_id.b.al_pa = entry[3];

			/* Last one exit. */
			if (entry[0] & BIT_7) {
				list[i].d_id.b.rsvd_1 = entry[0];
				break;
			}
		}

		/*
		 * If we've used all available slots, then the switch is
		 * reporting back more devices that we can handle with this
		 * single call.  Return a failed status, and let GA_NXT handle
		 * the overload.
		 */
		if (i == ha->max_fibre_devices)
			rval = QLA_FUNCTION_FAILED;
	}

	return (rval);
}

/**
 * qla2x00_sns_gpn_id() - SNS Get Port Name (GPN_ID) query.
 * @ha: HA context
 * @list: switch info entries to populate
 *
 * This command uses the old Exectute SNS Command mailbox routine.
 *
 * Returns 0 on success.
 */
static int
qla2x00_sns_gpn_id(scsi_qla_host_t *vha, sw_info_t *list)
{
	int		rval = QLA_SUCCESS;
	struct qla_hw_data *ha = vha->hw;
	uint16_t	i;
	struct sns_cmd_pkt	*sns_cmd;

	for (i = 0; i < ha->max_fibre_devices; i++) {
		/* Issue GPN_ID */
		/* Prepare SNS command request. */
		sns_cmd = qla2x00_prep_sns_cmd(vha, GPN_ID_CMD,
		    GPN_ID_SNS_SCMD_LEN, GPN_ID_SNS_DATA_SIZE);

		/* Prepare SNS command arguments -- port_id. */
		sns_cmd->p.cmd.param[0] = list[i].d_id.b.al_pa;
		sns_cmd->p.cmd.param[1] = list[i].d_id.b.area;
		sns_cmd->p.cmd.param[2] = list[i].d_id.b.domain;

		/* Execute SNS command. */
		rval = qla2x00_send_sns(vha, ha->sns_cmd_dma,
		    GPN_ID_SNS_CMD_SIZE / 2, sizeof(struct sns_cmd_pkt));
		if (rval != QLA_SUCCESS) {
			/*EMPTY*/
			ql_dbg(ql_dbg_disc, vha, 0x2032,
			    "GPN_ID Send SNS failed (%d).\n", rval);
		} else if (sns_cmd->p.gpn_data[8] != 0x80 ||
		    sns_cmd->p.gpn_data[9] != 0x02) {
			ql_dbg(ql_dbg_disc + ql_dbg_buffer, vha, 0x207e,
			    "GPN_ID failed, rejected request, gpn_rsp:\n");
			ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, vha, 0x207f,
			    sns_cmd->p.gpn_data, 16);
			rval = QLA_FUNCTION_FAILED;
		} else {
			/* Save portname */
			memcpy(list[i].port_name, &sns_cmd->p.gpn_data[16],
			    WWN_SIZE);
		}

		/* Last device exit. */
		if (list[i].d_id.b.rsvd_1 != 0)
			break;
	}

	return (rval);
}

/**
 * qla2x00_sns_gnn_id() - SNS Get Node Name (GNN_ID) query.
 * @ha: HA context
 * @list: switch info entries to populate
 *
 * This command uses the old Exectute SNS Command mailbox routine.
 *
 * Returns 0 on success.
 */
static int
qla2x00_sns_gnn_id(scsi_qla_host_t *vha, sw_info_t *list)
{
	int		rval = QLA_SUCCESS;
	struct qla_hw_data *ha = vha->hw;
	uint16_t	i;
	struct sns_cmd_pkt	*sns_cmd;

	for (i = 0; i < ha->max_fibre_devices; i++) {
		/* Issue GNN_ID */
		/* Prepare SNS command request. */
		sns_cmd = qla2x00_prep_sns_cmd(vha, GNN_ID_CMD,
		    GNN_ID_SNS_SCMD_LEN, GNN_ID_SNS_DATA_SIZE);

		/* Prepare SNS command arguments -- port_id. */
		sns_cmd->p.cmd.param[0] = list[i].d_id.b.al_pa;
		sns_cmd->p.cmd.param[1] = list[i].d_id.b.area;
		sns_cmd->p.cmd.param[2] = list[i].d_id.b.domain;

		/* Execute SNS command. */
		rval = qla2x00_send_sns(vha, ha->sns_cmd_dma,
		    GNN_ID_SNS_CMD_SIZE / 2, sizeof(struct sns_cmd_pkt));
		if (rval != QLA_SUCCESS) {
			/*EMPTY*/
			ql_dbg(ql_dbg_disc, vha, 0x203f,
			    "GNN_ID Send SNS failed (%d).\n", rval);
		} else if (sns_cmd->p.gnn_data[8] != 0x80 ||
		    sns_cmd->p.gnn_data[9] != 0x02) {
			ql_dbg(ql_dbg_disc + ql_dbg_buffer, vha, 0x2082,
			    "GNN_ID failed, rejected request, gnn_rsp:\n");
			ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, vha, 0x207a,
			    sns_cmd->p.gnn_data, 16);
			rval = QLA_FUNCTION_FAILED;
		} else {
			/* Save nodename */
			memcpy(list[i].node_name, &sns_cmd->p.gnn_data[16],
			    WWN_SIZE);

			ql_dbg(ql_dbg_disc, vha, 0x206e,
			    "GID_PT entry - nn %8phN pn %8phN port_id=%06x.\n",
			    list[i].node_name, list[i].port_name,
			    list[i].d_id.b24);
		}

		/* Last device exit. */
		if (list[i].d_id.b.rsvd_1 != 0)
			break;
	}

	return (rval);
}

/**
 * qla2x00_snd_rft_id() - SNS Register FC-4 TYPEs (RFT_ID) supported by the HBA.
 * @ha: HA context
 *
 * This command uses the old Exectute SNS Command mailbox routine.
 *
 * Returns 0 on success.
 */
static int
qla2x00_sns_rft_id(scsi_qla_host_t *vha)
{
	int		rval;
	struct qla_hw_data *ha = vha->hw;
	struct sns_cmd_pkt	*sns_cmd;

	/* Issue RFT_ID. */
	/* Prepare SNS command request. */
	sns_cmd = qla2x00_prep_sns_cmd(vha, RFT_ID_CMD, RFT_ID_SNS_SCMD_LEN,
	    RFT_ID_SNS_DATA_SIZE);

	/* Prepare SNS command arguments -- port_id, FC-4 types */
	sns_cmd->p.cmd.param[0] = vha->d_id.b.al_pa;
	sns_cmd->p.cmd.param[1] = vha->d_id.b.area;
	sns_cmd->p.cmd.param[2] = vha->d_id.b.domain;

	sns_cmd->p.cmd.param[5] = 0x01;			/* FCP-3 */

	/* Execute SNS command. */
	rval = qla2x00_send_sns(vha, ha->sns_cmd_dma, RFT_ID_SNS_CMD_SIZE / 2,
	    sizeof(struct sns_cmd_pkt));
	if (rval != QLA_SUCCESS) {
		/*EMPTY*/
		ql_dbg(ql_dbg_disc, vha, 0x2060,
		    "RFT_ID Send SNS failed (%d).\n", rval);
	} else if (sns_cmd->p.rft_data[8] != 0x80 ||
	    sns_cmd->p.rft_data[9] != 0x02) {
		ql_dbg(ql_dbg_disc + ql_dbg_buffer, vha, 0x2083,
		    "RFT_ID failed, rejected request rft_rsp:\n");
		ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, vha, 0x2080,
		    sns_cmd->p.rft_data, 16);
		rval = QLA_FUNCTION_FAILED;
	} else {
		ql_dbg(ql_dbg_disc, vha, 0x2073,
		    "RFT_ID exiting normally.\n");
	}

	return (rval);
}

/**
 * qla2x00_sns_rnn_id() - SNS Register Node Name (RNN_ID) of the HBA.
 * HBA.
 * @ha: HA context
 *
 * This command uses the old Exectute SNS Command mailbox routine.
 *
 * Returns 0 on success.
 */
static int
qla2x00_sns_rnn_id(scsi_qla_host_t *vha)
{
	int		rval;
	struct qla_hw_data *ha = vha->hw;
	struct sns_cmd_pkt	*sns_cmd;

	/* Issue RNN_ID. */
	/* Prepare SNS command request. */
	sns_cmd = qla2x00_prep_sns_cmd(vha, RNN_ID_CMD, RNN_ID_SNS_SCMD_LEN,
	    RNN_ID_SNS_DATA_SIZE);

	/* Prepare SNS command arguments -- port_id, nodename. */
	sns_cmd->p.cmd.param[0] = vha->d_id.b.al_pa;
	sns_cmd->p.cmd.param[1] = vha->d_id.b.area;
	sns_cmd->p.cmd.param[2] = vha->d_id.b.domain;

	sns_cmd->p.cmd.param[4] = vha->node_name[7];
	sns_cmd->p.cmd.param[5] = vha->node_name[6];
	sns_cmd->p.cmd.param[6] = vha->node_name[5];
	sns_cmd->p.cmd.param[7] = vha->node_name[4];
	sns_cmd->p.cmd.param[8] = vha->node_name[3];
	sns_cmd->p.cmd.param[9] = vha->node_name[2];
	sns_cmd->p.cmd.param[10] = vha->node_name[1];
	sns_cmd->p.cmd.param[11] = vha->node_name[0];

	/* Execute SNS command. */
	rval = qla2x00_send_sns(vha, ha->sns_cmd_dma, RNN_ID_SNS_CMD_SIZE / 2,
	    sizeof(struct sns_cmd_pkt));
	if (rval != QLA_SUCCESS) {
		/*EMPTY*/
		ql_dbg(ql_dbg_disc, vha, 0x204a,
		    "RNN_ID Send SNS failed (%d).\n", rval);
	} else if (sns_cmd->p.rnn_data[8] != 0x80 ||
	    sns_cmd->p.rnn_data[9] != 0x02) {
		ql_dbg(ql_dbg_disc + ql_dbg_buffer, vha, 0x207b,
		    "RNN_ID failed, rejected request, rnn_rsp:\n");
		ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, vha, 0x207c,
		    sns_cmd->p.rnn_data, 16);
		rval = QLA_FUNCTION_FAILED;
	} else {
		ql_dbg(ql_dbg_disc, vha, 0x204c,
		    "RNN_ID exiting normally.\n");
	}

	return (rval);
}

/**
 * qla2x00_mgmt_svr_login() - Login to fabric Management Service.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
static int
qla2x00_mgmt_svr_login(scsi_qla_host_t *vha)
{
	int rval;
	uint16_t mb[MAILBOX_REGISTER_COUNT];

	if (vha->flags.management_server_logged_in)
		return QLA_SUCCESS;

	rval = vha->hw->isp_ops->fabric_login(vha, vha->mgmt_svr_loop_id,
	    0xff, 0xff, 0xfa, mb, BIT_1|BIT_0);
	if (rval != QLA_SUCCESS || mb[0] != MBS_COMMAND_COMPLETE) {
		ql_dbg(ql_dbg_disc, vha, 0x2099,
		    "Failed management_server login: loopid=%x "
		    "mb[0]=%x mb[1]=%x mb[2]=%x mb[6]=%x mb[7]=%x (%x).\n",
		    vha->mgmt_svr_loop_id,
		    mb[0], mb[1], mb[2], mb[6], mb[7], rval);
		return QLA_FUNCTION_FAILED;
	}

	vha->flags.management_server_logged_in = 1;
	return QLA_SUCCESS;
}

/**
 * qla2x00_prep_ms_fdmi_iocb() - Prepare common MS IOCB fields for FDMI query.
 * @ha: HA context
 * @req_size: request size in bytes
 * @rsp_size: response size in bytes
 *
 * Returns a pointer to the @ha's ms_iocb.
 */
void *
qla2x00_prep_ms_fdmi_iocb(scsi_qla_host_t *vha, uint32_t req_size,
    uint32_t rsp_size)
{
	ms_iocb_entry_t *ms_pkt;
	struct qla_hw_data *ha = vha->hw;
	ms_pkt = ha->ms_iocb;
	memset(ms_pkt, 0, sizeof(ms_iocb_entry_t));

	ms_pkt->entry_type = MS_IOCB_TYPE;
	ms_pkt->entry_count = 1;
	SET_TARGET_ID(ha, ms_pkt->loop_id, vha->mgmt_svr_loop_id);
	ms_pkt->control_flags = __constant_cpu_to_le16(CF_READ | CF_HEAD_TAG);
	ms_pkt->timeout = cpu_to_le16(ha->r_a_tov / 10 * 2);
	ms_pkt->cmd_dsd_count = __constant_cpu_to_le16(1);
	ms_pkt->total_dsd_count = __constant_cpu_to_le16(2);
	ms_pkt->rsp_bytecount = cpu_to_le32(rsp_size);
	ms_pkt->req_bytecount = cpu_to_le32(req_size);

	ms_pkt->dseg_req_address[0] = cpu_to_le32(LSD(ha->ct_sns_dma));
	ms_pkt->dseg_req_address[1] = cpu_to_le32(MSD(ha->ct_sns_dma));
	ms_pkt->dseg_req_length = ms_pkt->req_bytecount;

	ms_pkt->dseg_rsp_address[0] = cpu_to_le32(LSD(ha->ct_sns_dma));
	ms_pkt->dseg_rsp_address[1] = cpu_to_le32(MSD(ha->ct_sns_dma));
	ms_pkt->dseg_rsp_length = ms_pkt->rsp_bytecount;

	return ms_pkt;
}

/**
 * qla24xx_prep_ms_fdmi_iocb() - Prepare common MS IOCB fields for FDMI query.
 * @ha: HA context
 * @req_size: request size in bytes
 * @rsp_size: response size in bytes
 *
 * Returns a pointer to the @ha's ms_iocb.
 */
void *
qla24xx_prep_ms_fdmi_iocb(scsi_qla_host_t *vha, uint32_t req_size,
    uint32_t rsp_size)
{
	struct ct_entry_24xx *ct_pkt;
	struct qla_hw_data *ha = vha->hw;

	ct_pkt = (struct ct_entry_24xx *)ha->ms_iocb;
	memset(ct_pkt, 0, sizeof(struct ct_entry_24xx));

	ct_pkt->entry_type = CT_IOCB_TYPE;
	ct_pkt->entry_count = 1;
	ct_pkt->nport_handle = cpu_to_le16(vha->mgmt_svr_loop_id);
	ct_pkt->timeout = cpu_to_le16(ha->r_a_tov / 10 * 2);
	ct_pkt->cmd_dsd_count = __constant_cpu_to_le16(1);
	ct_pkt->rsp_dsd_count = __constant_cpu_to_le16(1);
	ct_pkt->rsp_byte_count = cpu_to_le32(rsp_size);
	ct_pkt->cmd_byte_count = cpu_to_le32(req_size);

	ct_pkt->dseg_0_address[0] = cpu_to_le32(LSD(ha->ct_sns_dma));
	ct_pkt->dseg_0_address[1] = cpu_to_le32(MSD(ha->ct_sns_dma));
	ct_pkt->dseg_0_len = ct_pkt->cmd_byte_count;

	ct_pkt->dseg_1_address[0] = cpu_to_le32(LSD(ha->ct_sns_dma));
	ct_pkt->dseg_1_address[1] = cpu_to_le32(MSD(ha->ct_sns_dma));
	ct_pkt->dseg_1_len = ct_pkt->rsp_byte_count;
	ct_pkt->vp_index = vha->vp_idx;

	return ct_pkt;
}

static inline ms_iocb_entry_t *
qla2x00_update_ms_fdmi_iocb(scsi_qla_host_t *vha, uint32_t req_size)
{
	struct qla_hw_data *ha = vha->hw;
	ms_iocb_entry_t *ms_pkt = ha->ms_iocb;
	struct ct_entry_24xx *ct_pkt = (struct ct_entry_24xx *)ha->ms_iocb;

	if (IS_FWI2_CAPABLE(ha)) {
		ct_pkt->cmd_byte_count = cpu_to_le32(req_size);
		ct_pkt->dseg_0_len = ct_pkt->cmd_byte_count;
	} else {
		ms_pkt->req_bytecount = cpu_to_le32(req_size);
		ms_pkt->dseg_req_length = ms_pkt->req_bytecount;
	}

	return ms_pkt;
}

/**
 * qla2x00_prep_ct_req() - Prepare common CT request fields for SNS query.
 * @ct_req: CT request buffer
 * @cmd: GS command
 * @rsp_size: response size in bytes
 *
 * Returns a pointer to the intitialized @ct_req.
 */
static inline struct ct_sns_req *
qla2x00_prep_ct_fdmi_req(struct ct_sns_pkt *p, uint16_t cmd,
    uint16_t rsp_size)
{
	memset(p, 0, sizeof(struct ct_sns_pkt));

	p->p.req.header.revision = 0x01;
	p->p.req.header.gs_type = 0xFA;
	p->p.req.header.gs_subtype = 0x10;
	p->p.req.command = cpu_to_be16(cmd);
	p->p.req.max_rsp_size = cpu_to_be16((rsp_size - 16) / 4);

	return &p->p.req;
}

static ulong
qla2x00_hba_attributes(scsi_qla_host_t *vha, void *entries, uint callopt)
{
	struct qla_hw_data *ha = vha->hw;
	struct init_cb_24xx *icb24 = (void *)ha->init_cb;
	struct new_utsname *p_sysid = utsname();
	struct ct_fdmi_hba_attr *eiter;
	uint16_t alen;
	ulong size = 0;

	/* Nodename. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_NODE_NAME);
	memcpy(eiter->a.node_name, vha->node_name, sizeof(eiter->a.node_name));
	alen = sizeof(eiter->a.node_name);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20a0,
	    "NODENAME = %8phN.\n", eiter->a.node_name);

	/* Manufacturer. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_MANUFACTURER);
	alen = scnprintf(
	    eiter->a.manufacturer, sizeof(eiter->a.manufacturer),
	    "%s", "QLogic Corporation");
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20a1,
	    "MANUFACTURER = %s.\n", eiter->a.manufacturer);

	/* Serial number. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_SERIAL_NUMBER);
	alen = 0;
	if (IS_FWI2_CAPABLE(ha)) {
		alen = qla2xxx_get_vpd_field(vha, "SN",
		    eiter->a.serial_num, sizeof(eiter->a.serial_num));
	}
	if (!alen) {
		uint32_t sn = ((ha->serial0 & 0x1f) << 16) |
		    (ha->serial2 << 8) | ha->serial1;
		alen = scnprintf(
		    eiter->a.serial_num, sizeof(eiter->a.serial_num),
		    "%c%05d", 'A' + sn / 100000, sn % 100000);
	}
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20a2,
	    "SERIAL NUMBER = %s.\n", eiter->a.serial_num);

	/* Model name. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_MODEL);
	alen = scnprintf(
	    eiter->a.model, sizeof(eiter->a.model),
	    "%s", ha->model_number);
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20a3,
	    "MODEL NAME = %s.\n", eiter->a.model);

	/* Model description. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_MODEL_DESCRIPTION);
	alen = scnprintf(
	    eiter->a.model_desc, sizeof(eiter->a.model_desc),
	    "%s", ha->model_desc);
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20a4,
	    "MODEL DESCRIPTION = %s.\n", eiter->a.model_desc);

	/* Hardware version. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_HARDWARE_VERSION);
	alen = 0;
	if (IS_FWI2_CAPABLE(ha)) {
		if (!alen) {
			alen = qla2xxx_get_vpd_field(vha, "MN",
			    eiter->a.hw_version, sizeof(eiter->a.hw_version));
		}
		if (!alen) {
			alen = qla2xxx_get_vpd_field(vha, "EC",
			    eiter->a.hw_version, sizeof(eiter->a.hw_version));
		}
	}
	if (!alen) {
		alen = scnprintf(
		    eiter->a.hw_version, sizeof(eiter->a.hw_version),
		    "HW:%s", ha->adapter_id);
	}
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20a5,
	    "HARDWARE VERSION = %s.\n", eiter->a.hw_version);

	/* Driver version. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_DRIVER_VERSION);
	alen = scnprintf(
	    eiter->a.driver_version, sizeof(eiter->a.driver_version),
	    "%s", qla2x00_version_str);
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20a6,
	    "DRIVER VERSION = %s.\n", eiter->a.driver_version);

	/* Option ROM version. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_OPTION_ROM_VERSION);
	alen = scnprintf(
	    eiter->a.orom_version, sizeof(eiter->a.orom_version),
	    "%d.%02d", ha->bios_revision[1], ha->bios_revision[0]);
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20a7,
	    "OPTROM VERSION = %d.%02d.\n",
	    eiter->a.orom_version[1], eiter->a.orom_version[0]);

	/* Firmware version */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_FIRMWARE_VERSION);
	ha->isp_ops->fw_version_str(vha, eiter->a.fw_version,
	    sizeof(eiter->a.fw_version));
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20a8,
	    "FIRMWARE VERSION = %s.\n", eiter->a.fw_version);

	if (callopt == CALLOPT_FDMI1)
		goto done;

	/* OS Name and Version */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_OS_NAME_AND_VERSION);
	alen = 0;
	if (p_sysid) {
		alen = scnprintf(
		    eiter->a.os_version, sizeof(eiter->a.os_version),
		    "%s %s %s",
		    p_sysid->sysname, p_sysid->release, p_sysid->machine);
	}
	if (!alen) {
		alen = scnprintf(
		    eiter->a.os_version, sizeof(eiter->a.os_version),
		    "%s %s",
		    "Linux", fc_host_system_hostname(vha->host));
	}
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20a9,
	    "OS VERSION = %s.\n", eiter->a.os_version);

	/* MAX CT Payload Length */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_MAXIMUM_CT_PAYLOAD_LENGTH);
	eiter->a.max_ct_len = cpu_to_be32(le16_to_cpu(IS_FWI2_CAPABLE(ha) ?
	    icb24->frame_payload_size : ha->init_cb->frame_payload_size));
	alen = sizeof(eiter->a.max_ct_len);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20aa,
	    "CT PAYLOAD LENGTH = 0x%x.\n", be32_to_cpu(eiter->a.max_ct_len));

	/* Node Sybolic Name */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_NODE_SYMBOLIC_NAME);
	alen = qla2x00_get_sym_node_name(vha, eiter->a.sym_name,
	    sizeof(eiter->a.sym_name));
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20ab,
	    "SYMBOLIC NAME = %s.\n", eiter->a.sym_name);

	/* Vendor Specific information */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_VENDOR_SPECIFIC_INFO);
	eiter->a.vendor_specific_info = cpu_to_be32(PCI_VENDOR_ID_QLOGIC);
	alen = sizeof(eiter->a.vendor_specific_info);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20ac,
	    "VENDOR SPECIFIC INFO = 0x%x.\n",
	    be32_to_cpu(eiter->a.vendor_specific_info));

	/* Num Ports */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_NUM_PORTS);
	eiter->a.num_ports = cpu_to_be32(1);
	alen = sizeof(eiter->a.num_ports);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20ad,
	    "PORT COUNT = %x.\n", be32_to_cpu(eiter->a.num_ports));

	/* Fabric Name */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_FABRIC_NAME);
	memcpy(eiter->a.fabric_name, vha->fabric_node_name,
	    sizeof(eiter->a.fabric_name));
	alen = sizeof(eiter->a.fabric_name);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20ae,
	    "FABRIC NAME = %8phN.\n", eiter->a.fabric_name);

	/* BIOS Version */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_BOOT_BIOS_NAME);
	alen = scnprintf(
	    eiter->a.bios_name, sizeof(eiter->a.bios_name),
	    "BIOS %d.%02d", ha->bios_revision[1], ha->bios_revision[0]);
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20af,
	    "BIOS NAME = %s\n", eiter->a.bios_name);

	/* Vendor Identifier */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_HBA_VENDOR_IDENTIFIER);
	alen = scnprintf(
	    eiter->a.vendor_indentifer, sizeof(eiter->a.vendor_indentifer),
	    "%s", "QLGC");
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20b0,
	    "VENDOR IDENTIFIER = %s.\n", eiter->a.vendor_indentifer);

done:
	return size;
}

static ulong
qla2x00_port_atributes(scsi_qla_host_t *vha, void *entries, uint callopt)
{
	struct qla_hw_data *ha = vha->hw;
	struct init_cb_24xx *icb24 = (void *)ha->init_cb;
	struct new_utsname *p_sysid = utsname();
	char *hostname = p_sysid ?
	    p_sysid->nodename : fc_host_system_hostname(vha->host);
	struct ct_fdmi_port_attr *eiter;
	uint16_t alen;
	ulong size = 0;

	/* FC4 types. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_FC4_TYPES);
	eiter->a.fc4_types[0] = 0x00;
	eiter->a.fc4_types[1] = 0x00;
	eiter->a.fc4_types[2] = 0x01;
	eiter->a.fc4_types[3] = 0x00;
	alen = sizeof(eiter->a.fc4_types);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20c0,
	    "FC4 TYPES = %8phN.\n", eiter->a.fc4_types);

	/* Supported speed. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_SUPPORT_SPEED);
	if (IS_CNA_CAPABLE(ha))
		eiter->a.sup_speed = cpu_to_be32(
		    FDMI_PORT_SPEED_10GB);
	else if (IS_QLA27XX(ha))
		eiter->a.sup_speed = cpu_to_be32(
		    FDMI_PORT_SPEED_32GB|
		    FDMI_PORT_SPEED_16GB|
		    FDMI_PORT_SPEED_8GB);
	else if (IS_QLA2031(ha))
		eiter->a.sup_speed = cpu_to_be32(
		    FDMI_PORT_SPEED_16GB|
		    FDMI_PORT_SPEED_8GB|
		    FDMI_PORT_SPEED_4GB);
	else if (IS_QLA25XX(ha))
		eiter->a.sup_speed = cpu_to_be32(
		    FDMI_PORT_SPEED_8GB|
		    FDMI_PORT_SPEED_4GB|
		    FDMI_PORT_SPEED_2GB|
		    FDMI_PORT_SPEED_1GB);
	else if (IS_QLA24XX_TYPE(ha))
		eiter->a.sup_speed = cpu_to_be32(
		    FDMI_PORT_SPEED_4GB|
		    FDMI_PORT_SPEED_2GB|
		    FDMI_PORT_SPEED_1GB);
	else if (IS_QLA23XX(ha))
		eiter->a.sup_speed = cpu_to_be32(
		    FDMI_PORT_SPEED_2GB|
		    FDMI_PORT_SPEED_1GB);
	else
		eiter->a.sup_speed = cpu_to_be32(
		    FDMI_PORT_SPEED_1GB);
	alen = sizeof(eiter->a.sup_speed);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20c1,
	    "SUPPORTED SPEED = %x.\n", be32_to_cpu(eiter->a.sup_speed));

	/* Current speed. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_CURRENT_SPEED);
	switch (ha->link_data_rate) {
	case PORT_SPEED_1GB:
		eiter->a.cur_speed = cpu_to_be32(FDMI_PORT_SPEED_1GB);
		break;
	case PORT_SPEED_2GB:
		eiter->a.cur_speed = cpu_to_be32(FDMI_PORT_SPEED_2GB);
		break;
	case PORT_SPEED_4GB:
		eiter->a.cur_speed = cpu_to_be32(FDMI_PORT_SPEED_4GB);
		break;
	case PORT_SPEED_8GB:
		eiter->a.cur_speed = cpu_to_be32(FDMI_PORT_SPEED_8GB);
		break;
	case PORT_SPEED_10GB:
		eiter->a.cur_speed = cpu_to_be32(FDMI_PORT_SPEED_10GB);
		break;
	case PORT_SPEED_16GB:
		eiter->a.cur_speed = cpu_to_be32(FDMI_PORT_SPEED_16GB);
		break;
	case PORT_SPEED_32GB:
		eiter->a.cur_speed = cpu_to_be32(FDMI_PORT_SPEED_32GB);
		break;
	default:
		eiter->a.cur_speed = cpu_to_be32(FDMI_PORT_SPEED_UNKNOWN);
		break;
	}
	alen = sizeof(eiter->a.cur_speed);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20c2,
	    "CURRENT SPEED = %x.\n", be32_to_cpu(eiter->a.cur_speed));

	/* Max frame size. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_MAX_FRAME_SIZE);
	eiter->a.max_frame_size = cpu_to_be32(le16_to_cpu(IS_FWI2_CAPABLE(ha) ?
	    icb24->frame_payload_size : ha->init_cb->frame_payload_size));
	alen = sizeof(eiter->a.max_frame_size);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20c3,
	    "MAX FRAME SIZE = %x.\n", be32_to_cpu(eiter->a.max_frame_size));

	/* OS device name. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_OS_DEVICE_NAME);
	alen = scnprintf(
	    eiter->a.os_dev_name, sizeof(eiter->a.os_dev_name),
	    "%s:host%lu", QLA2XXX_DRIVER_NAME, vha->host_no);
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20c4,
	    "OS DEVICE NAME = %s.\n", eiter->a.os_dev_name);

	/* Hostname. */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_HOST_NAME);
	if (!*hostname || !strncmp(hostname, "(none)", 6))
		hostname = "Linux-default";
	alen = scnprintf(
	    eiter->a.host_name, sizeof(eiter->a.host_name),
	    "%s", hostname);
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20c5,
	    "HOSTNAME = %s.\n", eiter->a.host_name);

	if (callopt == CALLOPT_FDMI1)
		goto done;

	/* Node Name */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_NODE_NAME);
	memcpy(eiter->a.node_name, vha->node_name, sizeof(eiter->a.node_name));
	alen = sizeof(eiter->a.node_name);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20c6,
	    "NODENAME = %8phN.\n", eiter->a.node_name);

	/* Port Name */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_NAME);
	memcpy(eiter->a.port_name, vha->port_name, sizeof(eiter->a.port_name));
	alen = sizeof(eiter->a.port_name);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20c7,
	    "PORTNAME = %8phN.\n", eiter->a.port_name);

	/* Port Symbolic Name */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_SYM_NAME);
	alen = qla2x00_get_sym_node_name(vha, eiter->a.port_sym_name,
	    sizeof(eiter->a.port_sym_name));
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20c8,
	    "PORT SYMBOLIC NAME = %s\n", eiter->a.port_sym_name);

	/* Port Type */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_TYPE);
	eiter->a.port_type = cpu_to_be32(NS_NX_PORT_TYPE);
	alen = sizeof(eiter->a.port_type);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20c9,
	    "PORT TYPE = %x.\n", be32_to_cpu(eiter->a.port_type));

	/* Supported Class of Service */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_SUPP_COS);
	eiter->a.port_supported_cos = cpu_to_be32(FC_CLASS_3);
	alen = sizeof(eiter->a.port_supported_cos);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20ca,
	    "SUPPORTED COS = %08x\n", be32_to_cpu(eiter->a.port_supported_cos));

	/* Port Fabric Name */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_FABRIC_NAME);
	memcpy(eiter->a.fabric_name, vha->fabric_node_name,
	    sizeof(eiter->a.fabric_name));
	alen = sizeof(eiter->a.fabric_name);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20cb,
	    "FABRIC NAME = %8phN.\n", eiter->a.fabric_name);

	/* FC4_type */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_FC4_TYPE);
	eiter->a.port_fc4_type[0] = 0x00;
	eiter->a.port_fc4_type[1] = 0x00;
	eiter->a.port_fc4_type[2] = 0x01;
	eiter->a.port_fc4_type[3] = 0x00;
	alen = sizeof(eiter->a.port_fc4_type);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20cc,
	    "PORT ACTIVE FC4 TYPE = %8phN.\n", eiter->a.port_fc4_type);

	/* Port State */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_STATE);
	eiter->a.port_state = cpu_to_be32(2);
	alen = sizeof(eiter->a.port_state);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20cd,
	    "PORT_STATE = %x.\n", be32_to_cpu(eiter->a.port_state));

	/* Number of Ports */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_COUNT);
	eiter->a.num_ports = cpu_to_be32(1);
	alen = sizeof(eiter->a.num_ports);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20ce,
	    "PORT COUNT = %x.\n", be32_to_cpu(eiter->a.num_ports));

	/* Port Identifier */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_PORT_IDENTIFIER);
	eiter->a.port_id = cpu_to_be32(vha->d_id.b24);
	alen = sizeof(eiter->a.port_id);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20cf,
	    "PORT ID = %x.\n", be32_to_cpu(eiter->a.port_id));

	if (callopt == CALLOPT_FDMI2 || !ql2xsmartsan)
		goto done;

	/* Smart SAN Service Category (Populate Smart SAN Initiator)*/
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_SMARTSAN_SERVICE);
	alen = scnprintf(
	    eiter->a.smartsan_service, sizeof(eiter->a.smartsan_service),
	    "%s", "Smart SAN Initiator");
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20d0,
	    "SMARTSAN SERVICE CATEGORY = %s.\n", eiter->a.smartsan_service);

	/* Smart SAN GUID (NWWN+PWWN) */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_SMARTSAN_GUID);
	memcpy(eiter->a.smartsan_guid, vha->node_name, WWN_SIZE);
	memcpy(eiter->a.smartsan_guid + WWN_SIZE, vha->port_name, WWN_SIZE);
	alen = sizeof(eiter->a.smartsan_guid);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20d1,
	    "Smart SAN GUID = %8phN-%8phN\n",
	    eiter->a.smartsan_guid, eiter->a.smartsan_guid + WWN_SIZE);

	/* Smart SAN Version (populate "Smart SAN Version 1.0") */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_SMARTSAN_VERSION);
	alen = scnprintf(
	    eiter->a.smartsan_version, sizeof(eiter->a.smartsan_version),
	    "%s", "Smart SAN Version 1.0");
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20d2,
	    "SMARTSAN VERSION = %s\n", eiter->a.smartsan_version);

	/* Smart SAN Product Name (Specify Adapter Model No) */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_SMARTSAN_PROD_NAME);
	alen = scnprintf(
	    eiter->a.smartsan_prod_name, sizeof(eiter->a.smartsan_prod_name),
	    "ISP%04x", ha->pdev->device);
	alen += FDMI_ATTR_ALIGNMENT(alen);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20d3,
	    "SMARTSAN PRODUCT NAME = %s\n", eiter->a.smartsan_prod_name);

	/* Smart SAN Port Info (specify: 1=Physical, 2=NPIV, 3=SRIOV) */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_SMARTSAN_PORT_INFO);
	eiter->a.smartsan_port_info = cpu_to_be32(vha->vp_idx ? 2 : 1);
	alen = sizeof(eiter->a.smartsan_port_info);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20d4,
	    "SMARTSAN PORT INFO = %x\n", eiter->a.smartsan_port_info);

	/* Smart SAN QoS Support */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_SMARTSAN_QOS_SUPPORT);
	eiter->a.smartsan_qos_support = cpu_to_be32(QLA_QOS_NO_SUPP);
	alen = sizeof(eiter->a.smartsan_qos_support);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20d5,
	    "SMARTSAN QOS SUPPORT = %d\n",
	    be32_to_cpu(eiter->a.smartsan_qos_support));

	/* Smart SAN Security Support */
	eiter = entries + size;
	eiter->type = cpu_to_be16(FDMI_SMARTSAN_SECURITY_SUPPORT);
	eiter->a.smartsan_security_support = cpu_to_be32(1);
	alen = sizeof(eiter->a.smartsan_security_support);
	alen += FDMI_ATTR_TYPELEN(eiter);
	eiter->len = cpu_to_be16(alen);
	size += alen;

	ql_dbg(ql_dbg_disc, vha, 0x20d6,
	    "SMARTSAN SECURITY SUPPORT = %d\n",
	    be32_to_cpu(eiter->a.smartsan_security_support));

done:
	return size;
}

static int
qla2x00_fdmi_rhba(scsi_qla_host_t *vha, uint callopt)
{
	struct qla_hw_data *ha = vha->hw;
	ulong size = 0;
	uint rval, count;
	ms_iocb_entry_t *ms_pkt;
	struct ct_sns_req *ct_req;
	struct ct_sns_rsp *ct_rsp;
	void *entries;

	count =
	    callopt != CALLOPT_FDMI1 ?
		FDMI2_HBA_ATTR_COUNT : FDMI1_HBA_ATTR_COUNT;

	size = RHBA_RSP_SIZE;

	ql_dbg(ql_dbg_disc, vha, 0x20e0,
	    "RHBA (callopt=%x count=%u size=%lu).\n", callopt, count, size);

	/*   Request size adjusted after CT preparation */
	ms_pkt = ha->isp_ops->prep_ms_fdmi_iocb(vha, 0, size);

	/* Prepare CT request */
	ct_req = qla2x00_prep_ct_fdmi_req(ha->ct_sns, RHBA_CMD, size);
	ct_rsp = &ha->ct_sns->p.rsp;

	/* Prepare FDMI command entries */
	memcpy(ct_req->req.rhba.hba_identifier, vha->port_name,
	    sizeof(ct_req->req.rhba.hba_identifier));
	size += sizeof(ct_req->req.rhba.hba_identifier);

	ct_req->req.rhba.entry_count = cpu_to_be32(1);
	size += sizeof(ct_req->req.rhba.entry_count);

	memcpy(ct_req->req.rhba.port_name, vha->port_name,
	    sizeof(ct_req->req.rhba.port_name));
	size += sizeof(ct_req->req.rhba.port_name);

	/* Attribute count */
	ct_req->req.rhba.attrs.count = cpu_to_be32(count);
	size += sizeof(ct_req->req.rhba.attrs.count);

	/* Attribute block */
	entries = &ct_req->req.rhba.attrs.entry;

	size += qla2x00_hba_attributes(vha, entries, callopt);

	/* Update MS request size. */
	qla2x00_update_ms_fdmi_iocb(vha, size + 16);

	ql_dbg(ql_dbg_disc, vha, 0x20e1,
	    "RHBA %8phN %8phN.\n",
	    ct_req->req.rhba.hba_identifier, ct_req->req.rhba.port_name);

	ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, vha, 0x20e2,
	    entries, size);

	/* Execute MS IOCB */
	rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
	    sizeof(*ha->ms_iocb));
	if (rval) {
		ql_dbg(ql_dbg_disc, vha, 0x20e3,
		    "RHBA iocb failed (%d).\n", rval);
		return rval;
	}

	rval = qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp, "RHBA");
	if (rval) {
		if (ct_rsp->header.reason_code == CT_REASON_CANNOT_PERFORM &&
		    ct_rsp->header.explanation_code == CT_EXPL_ALREADY_REGISTERED) {
			ql_dbg(ql_dbg_disc, vha, 0x20e4,
			    "RHBA already registered.\n");
			return QLA_ALREADY_REGISTERED;
		}

		ql_dbg(ql_dbg_disc, vha, 0x20e5,
		    "RHBA failed, CT Reason %#x, CT Explanation %#x\n",
		    ct_rsp->header.reason_code,
		    ct_rsp->header.explanation_code);
		return rval;
	}

	ql_dbg(ql_dbg_disc, vha, 0x20e6, "RHBA exiting normally.\n");
	return rval;
}

static int
qla2x00_fdmi_dhba(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;
	int rval;
	uint32_t size = 0;
	ms_iocb_entry_t *ms_pkt;
	struct ct_sns_req *ct_req;
	struct ct_sns_rsp *ct_rsp;

	size = DHBA_RSP_SIZE;

	ql_dbg(ql_dbg_disc, vha, 0x209a, "DHBA.\n");

	/* Prepare common MS IOCB */
	ms_pkt = ha->isp_ops->prep_ms_fdmi_iocb(vha, DHBA_REQ_SIZE, size);

	/* Prepare CT request */
	ct_req = qla2x00_prep_ct_fdmi_req(ha->ct_sns, DHBA_CMD, size);
	ct_rsp = &ha->ct_sns->p.rsp;

	/* Prepare FDMI command entry */
	memcpy(ct_req->req.dhba.port_name, vha->port_name,
	    sizeof(ct_req->req.dhba.port_name));

	ql_dbg(ql_dbg_disc, vha, 0x209b,
	    "DHBA %8phN.\n", ct_req->req.dhba.port_name);

	/* Execute MS IOCB */
	rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
	    sizeof(*ha->ms_iocb));
	if (rval) {
		ql_dbg(ql_dbg_disc, vha, 0x209c,
		    "DHBA iocb failed (%d).\n", rval);
		return rval;
	}

	rval = qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp, "DHBA");
	if (rval) {
		ql_dbg(ql_dbg_disc, vha, 0x209d,
		    "DHBA failed, CT Reason %#x, CT Explanation %#x\n",
		    ct_rsp->header.reason_code,
		    ct_rsp->header.explanation_code);
		return rval;
	}

	ql_dbg(ql_dbg_disc, vha, 0x209e, "DHBA exiting normally.\n");
	return rval;
}

static int
qla2x00_fdmi_rprt(scsi_qla_host_t *vha, int callopt)
{
	struct scsi_qla_host *base_vha = pci_get_drvdata(vha->hw->pdev);
	struct qla_hw_data *ha = vha->hw;
	ulong size = 0;
	uint rval, count;
	ms_iocb_entry_t *ms_pkt;
	struct ct_sns_req *ct_req;
	struct ct_sns_rsp *ct_rsp;
	void *entries;

	count =
	    callopt == CALLOPT_FDMI2_SMARTSAN && ql2xsmartsan ?
		FDMI2_SMARTSAN_PORT_ATTR_COUNT :
	    callopt != CALLOPT_FDMI1 ?
		FDMI2_PORT_ATTR_COUNT : FDMI1_PORT_ATTR_COUNT;

	size = RPRT_RSP_SIZE;

	ql_dbg(ql_dbg_disc, vha, 0x20e8,
	    "RPRT (callopt=%x count=%u size=%lu).\n", callopt, count, size);

	/* Request size adjusted after CT preparation */
	ms_pkt = ha->isp_ops->prep_ms_fdmi_iocb(vha, 0, size);

	/* Prepare CT request */
	ct_req = qla2x00_prep_ct_fdmi_req(ha->ct_sns, RPRT_CMD, size);
	ct_rsp = &ha->ct_sns->p.rsp;

	/* Prepare FDMI command entries */
	memcpy(ct_req->req.rprt.hba_identifier, base_vha->port_name,
	    sizeof(ct_req->req.rprt.hba_identifier));
	size += sizeof(ct_req->req.rprt.hba_identifier);

	memcpy(ct_req->req.rprt.port_name, vha->port_name,
	    sizeof(ct_req->req.rprt.port_name));
	size += sizeof(ct_req->req.rprt.port_name);

	/* Attribute count */
	ct_req->req.rprt.attrs.count = cpu_to_be32(count);
	size += sizeof(ct_req->req.rprt.attrs.count);

	/* Attribute block */
	entries = ct_req->req.rprt.attrs.entry;

	size += qla2x00_port_atributes(vha, entries, callopt);

	/* Update MS request size. */
	qla2x00_update_ms_fdmi_iocb(vha, size + 16);

	ql_dbg(ql_dbg_disc, vha, 0x20e9,
	    "RPRT %8phN  %8phN.\n",
	    ct_req->req.rprt.port_name, ct_req->req.rprt.port_name);

	ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, vha, 0x20ea,
	    entries, size);

	/* Execute MS IOCB */
	rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
	    sizeof(*ha->ms_iocb));
	if (rval) {
		ql_dbg(ql_dbg_disc, vha, 0x20eb,
		    "RPRT iocb failed (%d).\n", rval);
		return rval;
	}

	rval = qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp, "RPRT");
	if (rval) {
		if (ct_rsp->header.reason_code == CT_REASON_CANNOT_PERFORM &&
		    ct_rsp->header.explanation_code == CT_EXPL_ALREADY_REGISTERED) {
			ql_dbg(ql_dbg_disc, vha, 0x20ec,
			    "RPRT already registered.\n");
			return QLA_ALREADY_REGISTERED;
		}

		ql_dbg(ql_dbg_disc, vha, 0x20ed,
		    "RPRT failed, CT Reason code: %#x, CT Explanation %#x\n",
		    ct_rsp->header.reason_code,
		    ct_rsp->header.explanation_code);
		return rval;
	}

	ql_dbg(ql_dbg_disc, vha, 0x20ee, "RPRT exiting normally.\n");
	return rval;
}

static int
qla2x00_fdmi_rpa(scsi_qla_host_t *vha, uint callopt)
{
	struct qla_hw_data *ha = vha->hw;
	ulong size = 0;
	uint rval, count;
	ms_iocb_entry_t *ms_pkt;
	struct ct_sns_req *ct_req;
	struct ct_sns_rsp *ct_rsp;
	void *entries;

	count =
	    callopt == CALLOPT_FDMI2_SMARTSAN && ql2xsmartsan ?
		FDMI2_SMARTSAN_PORT_ATTR_COUNT :
	    callopt != CALLOPT_FDMI1 ?
		FDMI2_PORT_ATTR_COUNT : FDMI1_PORT_ATTR_COUNT;

	size =
	    callopt != CALLOPT_FDMI1 ?
		SMARTSAN_RPA_RSP_SIZE : RPA_RSP_SIZE;

	ql_dbg(ql_dbg_disc, vha, 0x20f0,
	    "RPA (callopt=%x count=%u size=%lu).\n", callopt, count, size);

	/* Request size adjusted after CT preparation */
	ms_pkt = ha->isp_ops->prep_ms_fdmi_iocb(vha, 0, size);

	/* Prepare CT request */
	ct_req = qla2x00_prep_ct_fdmi_req(ha->ct_sns, RPA_CMD, size);
	ct_rsp = &ha->ct_sns->p.rsp;

	/* Prepare FDMI command entries. */
	memcpy(ct_req->req.rpa.port_name, vha->port_name,
	    sizeof(ct_req->req.rpa.port_name));
	size += sizeof(ct_req->req.rpa.port_name);

	/* Attribute count */
	ct_req->req.rpa.attrs.count = cpu_to_be32(count);
	size += sizeof(ct_req->req.rpa.attrs.count);

	/* Attribute block */
	entries = ct_req->req.rpa.attrs.entry;

	size += qla2x00_port_atributes(vha, entries, callopt);

	/* Update MS request size. */
	qla2x00_update_ms_fdmi_iocb(vha, size + 16);

	ql_dbg(ql_dbg_disc, vha, 0x20f1,
	    "RPA %8phN.\n", ct_req->req.rpa.port_name);

	ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, vha, 0x20f2,
	    entries, size);

	/* Execute MS IOCB */
	rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
	    sizeof(*ha->ms_iocb));
	if (rval) {
		ql_dbg(ql_dbg_disc, vha, 0x20f3,
		    "RPA iocb failed (%d).\n", rval);
		return rval;
	}

	rval = qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp, "RPA");
	if (rval) {
		if (ct_rsp->header.reason_code == CT_REASON_CANNOT_PERFORM &&
		    ct_rsp->header.explanation_code == CT_EXPL_ALREADY_REGISTERED) {
			ql_dbg(ql_dbg_disc, vha, 0x20f4,
			    "RPA already registered.\n");
			return QLA_ALREADY_REGISTERED;
		}

		ql_dbg(ql_dbg_disc, vha, 0x20f5,
		    "RPA failed, CT Reason code: %#x, CT Explanation %#x\n",
		    ct_rsp->header.reason_code,
		    ct_rsp->header.explanation_code);
		return rval;
	}

	ql_dbg(ql_dbg_disc, vha, 0x20f6, "RPA exiting normally.\n");
	return rval;
}

int
qla2x00_fdmi_register(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;
	int rval = QLA_SUCCESS;

	if (IS_QLA2100(ha) || IS_QLA2200(ha) || IS_QLAFX00(ha))
		return rval;

	rval = qla2x00_mgmt_svr_login(vha);
	if (rval)
		return rval;

	/* For npiv/vport send rprt only */
	if (vha->vp_idx) {
		if (ql2xsmartsan)
			rval = qla2x00_fdmi_rprt(vha, CALLOPT_FDMI2_SMARTSAN);
		if (rval || !ql2xsmartsan)
			rval = qla2x00_fdmi_rprt(vha, CALLOPT_FDMI2);
		if (rval)
			rval = qla2x00_fdmi_rprt(vha, CALLOPT_FDMI1);

		return rval;
	}

	/* Try fdmi2 first, if fails then try fdmi1 */
	rval = qla2x00_fdmi_rhba(vha, CALLOPT_FDMI2);
	if (rval) {
		if (rval != QLA_ALREADY_REGISTERED)
			goto try_fdmi;

		rval = qla2x00_fdmi_dhba(vha);
		if (rval)
			goto try_fdmi;

		rval = qla2x00_fdmi_rhba(vha, CALLOPT_FDMI2);
		if (rval)
			goto try_fdmi;
	}

	if (ql2xsmartsan)
		rval = qla2x00_fdmi_rpa(vha, CALLOPT_FDMI2_SMARTSAN);
	if (rval || !ql2xsmartsan)
		rval = qla2x00_fdmi_rpa(vha, CALLOPT_FDMI2);
	if (rval)
		goto try_fdmi;

	return rval;

try_fdmi:
	rval = qla2x00_fdmi_rhba(vha, CALLOPT_FDMI1);
	if (rval) {
		if (rval != QLA_ALREADY_REGISTERED)
			return rval;

		rval = qla2x00_fdmi_dhba(vha);
		if (rval)
			return rval;

		rval = qla2x00_fdmi_rhba(vha, CALLOPT_FDMI1);
		if (rval)
			return rval;
	}

	rval = qla2x00_fdmi_rpa(vha, CALLOPT_FDMI1);

	return rval;
}

/**
 * qla2x00_gfpn_id() - SNS Get Fabric Port Name (GFPN_ID) query.
 * @ha: HA context
 * @list: switch info entries to populate
 *
 * Returns 0 on success.
 */
int
qla2x00_gfpn_id(scsi_qla_host_t *vha, sw_info_t *list)
{
	int		rval = QLA_SUCCESS;
	uint16_t	i;
	struct qla_hw_data *ha = vha->hw;
	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;

	if (!IS_IIDMA_CAPABLE(ha))
		return QLA_FUNCTION_FAILED;

	for (i = 0; i < ha->max_fibre_devices; i++) {
		/* Issue GFPN_ID */
		/* Prepare common MS IOCB */
		ms_pkt = ha->isp_ops->prep_ms_iocb(vha, GFPN_ID_REQ_SIZE,
		    GFPN_ID_RSP_SIZE);

		/* Prepare CT request */
		ct_req = qla2x00_prep_ct_req(ha->ct_sns, GFPN_ID_CMD,
		    GFPN_ID_RSP_SIZE);
		ct_rsp = &ha->ct_sns->p.rsp;

		/* Prepare CT arguments -- port_id */
		ct_req->req.port_id.port_id[0] = list[i].d_id.b.domain;
		ct_req->req.port_id.port_id[1] = list[i].d_id.b.area;
		ct_req->req.port_id.port_id[2] = list[i].d_id.b.al_pa;

		/* Execute MS IOCB */
		rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
		    sizeof(ms_iocb_entry_t));
		if (rval != QLA_SUCCESS) {
			/*EMPTY*/
			ql_dbg(ql_dbg_disc, vha, 0x2023,
			    "GFPN_ID issue IOCB failed (%d).\n", rval);
			break;
		} else if (qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp,
		    "GFPN_ID") != QLA_SUCCESS) {
			rval = QLA_FUNCTION_FAILED;
			break;
		} else {
			/* Save fabric portname */
			memcpy(list[i].fabric_port_name,
			    ct_rsp->rsp.gfpn_id.port_name, WWN_SIZE);
		}

		/* Last device exit. */
		if (list[i].d_id.b.rsvd_1 != 0)
			break;
	}

	return (rval);
}

static inline void *
qla24xx_prep_ms_fm_iocb(scsi_qla_host_t *vha, uint32_t req_size,
    uint32_t rsp_size)
{
	struct ct_entry_24xx *ct_pkt;
	struct qla_hw_data *ha = vha->hw;
	ct_pkt = (struct ct_entry_24xx *)ha->ms_iocb;
	memset(ct_pkt, 0, sizeof(struct ct_entry_24xx));

	ct_pkt->entry_type = CT_IOCB_TYPE;
	ct_pkt->entry_count = 1;
	ct_pkt->nport_handle = cpu_to_le16(vha->mgmt_svr_loop_id);
	ct_pkt->timeout = cpu_to_le16(ha->r_a_tov / 10 * 2);
	ct_pkt->cmd_dsd_count = __constant_cpu_to_le16(1);
	ct_pkt->rsp_dsd_count = __constant_cpu_to_le16(1);
	ct_pkt->rsp_byte_count = cpu_to_le32(rsp_size);
	ct_pkt->cmd_byte_count = cpu_to_le32(req_size);

	ct_pkt->dseg_0_address[0] = cpu_to_le32(LSD(ha->ct_sns_dma));
	ct_pkt->dseg_0_address[1] = cpu_to_le32(MSD(ha->ct_sns_dma));
	ct_pkt->dseg_0_len = ct_pkt->cmd_byte_count;

	ct_pkt->dseg_1_address[0] = cpu_to_le32(LSD(ha->ct_sns_dma));
	ct_pkt->dseg_1_address[1] = cpu_to_le32(MSD(ha->ct_sns_dma));
	ct_pkt->dseg_1_len = ct_pkt->rsp_byte_count;
	ct_pkt->vp_index = vha->vp_idx;

	return ct_pkt;
}


static inline struct ct_sns_req *
qla24xx_prep_ct_fm_req(struct ct_sns_pkt *p, uint16_t cmd,
    uint16_t rsp_size)
{
	memset(p, 0, sizeof(struct ct_sns_pkt));

	p->p.req.header.revision = 0x01;
	p->p.req.header.gs_type = 0xFA;
	p->p.req.header.gs_subtype = 0x01;
	p->p.req.command = cpu_to_be16(cmd);
	p->p.req.max_rsp_size = cpu_to_be16((rsp_size - 16) / 4);

	return &p->p.req;
}

/**
 * qla2x00_gpsc() - FCS Get Port Speed Capabilities (GPSC) query.
 * @ha: HA context
 * @list: switch info entries to populate
 *
 * Returns 0 on success.
 */
int
qla2x00_gpsc(scsi_qla_host_t *vha, sw_info_t *list)
{
	int		rval;
	uint16_t	i;
	struct qla_hw_data *ha = vha->hw;
	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;

	if (!IS_IIDMA_CAPABLE(ha))
		return QLA_FUNCTION_FAILED;
	if (!ha->flags.gpsc_supported)
		return QLA_FUNCTION_FAILED;

	rval = qla2x00_mgmt_svr_login(vha);
	if (rval)
		return rval;

	for (i = 0; i < ha->max_fibre_devices; i++) {
		/* Issue GFPN_ID */
		/* Prepare common MS IOCB */
		ms_pkt = qla24xx_prep_ms_fm_iocb(vha, GPSC_REQ_SIZE,
		    GPSC_RSP_SIZE);

		/* Prepare CT request */
		ct_req = qla24xx_prep_ct_fm_req(ha->ct_sns, GPSC_CMD,
		    GPSC_RSP_SIZE);
		ct_rsp = &ha->ct_sns->p.rsp;

		/* Prepare CT arguments -- port_name */
		memcpy(ct_req->req.gpsc.port_name, list[i].fabric_port_name,
		    WWN_SIZE);

		/* Execute MS IOCB */
		rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
		    sizeof(ms_iocb_entry_t));
		if (rval != QLA_SUCCESS) {
			/*EMPTY*/
			ql_dbg(ql_dbg_disc, vha, 0x2059,
			    "GPSC issue IOCB failed (%d).\n", rval);
		} else if ((rval = qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp,
		    "GPSC")) != QLA_SUCCESS) {
			/* FM command unsupported? */
			if (rval == QLA_INVALID_COMMAND &&
			    (ct_rsp->header.reason_code ==
				CT_REASON_INVALID_COMMAND_CODE ||
			     ct_rsp->header.reason_code ==
				CT_REASON_COMMAND_UNSUPPORTED)) {
				ql_dbg(ql_dbg_disc, vha, 0x205a,
				    "GPSC command unsupported, disabling "
				    "query.\n");
				ha->flags.gpsc_supported = 0;
				rval = QLA_FUNCTION_FAILED;
				break;
			}
			rval = QLA_FUNCTION_FAILED;
		} else {
			/* Save port-speed */
			switch (be16_to_cpu(ct_rsp->rsp.gpsc.speed)) {
			case BIT_15:
				list[i].fp_speed = PORT_SPEED_1GB;
				break;
			case BIT_14:
				list[i].fp_speed = PORT_SPEED_2GB;
				break;
			case BIT_13:
				list[i].fp_speed = PORT_SPEED_4GB;
				break;
			case BIT_12:
				list[i].fp_speed = PORT_SPEED_10GB;
				break;
			case BIT_11:
				list[i].fp_speed = PORT_SPEED_8GB;
				break;
			case BIT_10:
				list[i].fp_speed = PORT_SPEED_16GB;
				break;
			case BIT_8:
				list[i].fp_speed = PORT_SPEED_32GB;
				break;
			}

			ql_dbg(ql_dbg_disc, vha, 0x205b,
			    "GPSC ext entry - fpn %8phN "
			    "speeds=%04x speed=%04x.\n",
			    list[i].fabric_port_name,
			    be16_to_cpu(ct_rsp->rsp.gpsc.speeds),
			    be16_to_cpu(ct_rsp->rsp.gpsc.speed));
		}

		/* Last device exit. */
		if (list[i].d_id.b.rsvd_1 != 0)
			break;
	}

	return (rval);
}

/**
 * qla2x00_gff_id() - SNS Get FC-4 Features (GFF_ID) query.
 *
 * @ha: HA context
 * @list: switch info entries to populate
 *
 */
void
qla2x00_gff_id(scsi_qla_host_t *vha, sw_info_t *list)
{
	int		rval;
	uint16_t	i;

	ms_iocb_entry_t	*ms_pkt;
	struct ct_sns_req	*ct_req;
	struct ct_sns_rsp	*ct_rsp;
	struct qla_hw_data *ha = vha->hw;
	uint8_t fcp_scsi_features = 0;

	for (i = 0; i < ha->max_fibre_devices; i++) {
		/* Set default FC4 Type as UNKNOWN so the default is to
		 * Process this port */
		list[i].fc4_type = FC4_TYPE_UNKNOWN;

		/* Do not attempt GFF_ID if we are not FWI_2 capable */
		if (!IS_FWI2_CAPABLE(ha))
			continue;

		/* Prepare common MS IOCB */
		ms_pkt = ha->isp_ops->prep_ms_iocb(vha, GFF_ID_REQ_SIZE,
		    GFF_ID_RSP_SIZE);

		/* Prepare CT request */
		ct_req = qla2x00_prep_ct_req(ha->ct_sns, GFF_ID_CMD,
		    GFF_ID_RSP_SIZE);
		ct_rsp = &ha->ct_sns->p.rsp;

		/* Prepare CT arguments -- port_id */
		ct_req->req.port_id.port_id[0] = list[i].d_id.b.domain;
		ct_req->req.port_id.port_id[1] = list[i].d_id.b.area;
		ct_req->req.port_id.port_id[2] = list[i].d_id.b.al_pa;

		/* Execute MS IOCB */
		rval = qla2x00_issue_iocb(vha, ha->ms_iocb, ha->ms_iocb_dma,
		   sizeof(ms_iocb_entry_t));

		if (rval != QLA_SUCCESS) {
			ql_dbg(ql_dbg_disc, vha, 0x205c,
			    "GFF_ID issue IOCB failed (%d).\n", rval);
		} else if (qla2x00_chk_ms_status(vha, ms_pkt, ct_rsp,
			       "GFF_ID") != QLA_SUCCESS) {
			ql_dbg(ql_dbg_disc, vha, 0x205d,
			    "GFF_ID IOCB status had a failure status code.\n");
		} else {
			fcp_scsi_features =
			   ct_rsp->rsp.gff_id.fc4_features[GFF_FCP_SCSI_OFFSET];
			fcp_scsi_features &= 0x0f;

			if (fcp_scsi_features)
				list[i].fc4_type = FC4_TYPE_FCP_SCSI;
			else
				list[i].fc4_type = FC4_TYPE_OTHER;
		}

		/* Last device exit. */
		if (list[i].d_id.b.rsvd_1 != 0)
			break;
	}
}
