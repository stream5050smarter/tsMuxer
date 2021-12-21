﻿#include "vvc.h"

#include <fs/systemlog.h>
#include <math.h>

#include <algorithm>

#include "tsMuxer.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace std;
static const int EXTENDED_SAR = 255;

// ------------------------- VvcUnit -------------------

unsigned VvcUnit::extractUEGolombCode()
{
    int cnt = 0;
    for (; m_reader.getBits(1) == 0; cnt++)
        ;
    if (cnt > INT_BIT)
        THROW_BITSTREAM_ERR;
    return (1 << cnt) - 1 + m_reader.getBits(cnt);
}

int VvcUnit::extractSEGolombCode()
{
    unsigned rez = extractUEGolombCode();
    if (rez % 2 == 0)
        return -(int)(rez / 2);
    else
        return (int)((rez + 1) / 2);
}

void VvcUnit::decodeBuffer(const uint8_t* buffer, const uint8_t* end)
{
    delete[] m_nalBuffer;
    m_nalBuffer = new uint8_t[end - buffer];
    m_nalBufferLen = NALUnit::decodeNAL(buffer, end, m_nalBuffer, end - buffer);
}

int VvcUnit::deserialize()
{
    m_reader.setBuffer(m_nalBuffer, m_nalBuffer + m_nalBufferLen);
    try
    {
        m_reader.skipBits(2);  // forbidden_zero_bit, nuh_reserved_zero_bit
        nuh_layer_id = m_reader.getBits(6);
        nal_unit_type = m_reader.getBits(5);
        nuh_temporal_id_plus1 = m_reader.getBits(3);
        if (nuh_temporal_id_plus1 == 0 || (nuh_temporal_id_plus1 != 1 && ((nal_unit_type >= 7 && nal_unit_type <= 15) ||
                                                                          nal_unit_type == 21 || nal_unit_type == 22)))
            return 1;
        return 0;
    }
    catch (BitStreamException& e)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

void VvcUnit::updateBits(int bitOffset, int bitLen, int value)
{
    uint8_t* ptr = (uint8_t*)m_reader.getBuffer() + bitOffset / 8;
    BitStreamWriter bitWriter;
    int byteOffset = bitOffset % 8;
    bitWriter.setBuffer(ptr, ptr + (bitLen / 8 + 5));

    uint8_t* ptr_end = (uint8_t*)m_reader.getBuffer() + (bitOffset + bitLen) / 8;
    int endBitsPostfix = 8 - ((bitOffset + bitLen) % 8);

    if (byteOffset > 0)
    {
        int prefix = *ptr >> (8 - byteOffset);
        bitWriter.putBits(byteOffset, prefix);
    }
    bitWriter.putBits(bitLen, value);

    if (endBitsPostfix < 8)
    {
        int postfix = *ptr_end & (1 << endBitsPostfix) - 1;
        bitWriter.putBits(endBitsPostfix, postfix);
    }
    bitWriter.flushBits();
}

int VvcUnit::serializeBuffer(uint8_t* dstBuffer, uint8_t* dstEnd) const
{
    if (m_nalBufferLen == 0)
        return 0;
    int encodeRez = NALUnit::encodeNAL(m_nalBuffer, m_nalBuffer + m_nalBufferLen, dstBuffer, dstEnd - dstBuffer);
    if (encodeRez == -1)
        return -1;
    else
        return encodeRez;
}

bool VvcUnit::dpb_parameters(int MaxSubLayersMinus1, bool subLayerInfoFlag)
{
    for (int i = (subLayerInfoFlag ? 0 : MaxSubLayersMinus1); i <= MaxSubLayersMinus1; i++)
    {
        unsigned dpb_max_dec_pic_buffering_minus1 = extractUEGolombCode();
        unsigned dpb_max_num_reorder_pics = extractUEGolombCode();
        if (dpb_max_num_reorder_pics > dpb_max_dec_pic_buffering_minus1)
            return 1;
        unsigned dpb_max_latency_increase_plus1 = extractUEGolombCode();
        if (dpb_max_latency_increase_plus1 == 0xffffffff)
            return 1;
    }
    return 0;
}

// ------------------------- VvcUnitWithProfile  -------------------

VvcUnitWithProfile::VvcUnitWithProfile() : profile_idc(0), level_idc(0) {}

int VvcUnitWithProfile::profile_tier_level(bool profileTierPresentFlag, int MaxNumSubLayersMinus1)
{
    try
    {
        if (profileTierPresentFlag)
        {
            profile_idc = m_reader.getBits(7);
            bool tier_flag = m_reader.getBit();
        }
        level_idc = m_reader.getBits(8);
        m_reader.skipBits(2);  // ptl_frame_only_constraint_flag, ptl_multilayer_enabled_flag

        if (profileTierPresentFlag)
        {                           // general_constraints_info()
            if (m_reader.getBit())  // gci_present_flag
            {
                m_reader.skipBits(32);
                m_reader.skipBits(32);
                m_reader.skipBits(7);
                int gci_num_reserved_bits = m_reader.getBits(8);
                for (int i = 0; i < gci_num_reserved_bits; i++) m_reader.skipBit();  // gci_reserved_zero_bit[i]
            }
            m_reader.skipBits(m_reader.getBitsLeft() % 8);  // gci_alignment_zero_bit
        }
        std::vector<int> ptl_sublayer_level_present_flag;
        ptl_sublayer_level_present_flag.resize(MaxNumSubLayersMinus1);

        for (int i = MaxNumSubLayersMinus1 - 1; i >= 0; i--) ptl_sublayer_level_present_flag[i] = m_reader.getBit();

        m_reader.skipBits(m_reader.getBitsLeft() % 8);  // ptl_reserved_zero_bit

        for (int i = MaxNumSubLayersMinus1 - 1; i >= 0; i--)
            if (ptl_sublayer_level_present_flag[i])
                m_reader.skipBits(8);  // sublayer_level_idc[i]
        if (profileTierPresentFlag)
        {
            int ptl_num_sub_profiles = m_reader.getBits(8);
            for (int i = 0; i < ptl_num_sub_profiles; i++) m_reader.skipBits(32);  // general_sub_profile_idc[i]
        }
        return 0;
    }
    catch (BitStreamException& e)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

std::string VvcUnitWithProfile::getProfileString() const
{
    string rez("Profile: ");
    if (profile_idc == 1)
        rez += string("Main10");
    else if (profile_idc == 65)
        rez += string("Main10StillPicture");
    else if (profile_idc == 33)
        rez += string("Main10_4:4:4");
    else if (profile_idc == 97)
        rez += string("Main10_4:4:4_StillPicture");
    else if (profile_idc == 17)
        rez += string("Main10_Multilayer");
    else if (profile_idc == 49)
        rez += string("Main10_Multilayer_4:4:4");
    else if (profile_idc == 0)
        rez += string("Not defined");
    else
        rez += "Unknown";
    if (level_idc)
    {
        rez += string("@");
        rez += int32ToStr(level_idc / 16);
        rez += string(".");
        rez += int32ToStr((level_idc % 16) / 3);
    }
    return rez;
}

// ------------------------- VvcVpsUnit -------------------

VvcVpsUnit::VvcVpsUnit()
    : VvcUnitWithProfile(),
      vps_id(0),
      vps_max_layers(0),
      vps_max_sublayers(0),
      num_units_in_tick(0),
      time_scale(0),
      num_units_in_tick_bit_pos(-1)
{
}

int VvcVpsUnit::deserialize()
{
    int rez = VvcUnit::deserialize();
    if (rez)
        return rez;
    try
    {
        vps_id = m_reader.getBits(4);
        vps_max_layers = m_reader.getBits(6) + 1;
        vps_max_sublayers = m_reader.getBits(3) + 1;
        bool vps_default_ptl_dpb_hrd_max_tid_flag =
            (vps_max_layers > 1 && vps_max_sublayers > 1) ? m_reader.getBit() : 1;
        int vps_all_independent_layers_flag = (vps_max_layers > 1) ? m_reader.getBit() : 1;
        for (int i = 0; i < vps_max_layers; i++)
        {
            m_reader.skipBits(6);  // vps_layer_id[i]
            if (i > 0 && !vps_all_independent_layers_flag)
            {
                if (!m_reader.getBit())  // vps_independent_layer_flag[i]
                {
                    bool vps_max_tid_ref_present_flag = m_reader.getBit();
                    for (int j = 0; j < i; j++)
                    {
                        bool vps_direct_ref_layer_flag = m_reader.getBit();
                        if (vps_max_tid_ref_present_flag && vps_direct_ref_layer_flag)
                            m_reader.skipBits(3);  // vps_max_tid_il_ref_pics_plus1[i][j]
                    }
                }
            }
        }

        bool vps_each_layer_is_an_ols_flag = 1;
        int vps_num_ptls = 1;
        int vps_ols_mode_idc = 2;
        int olsModeIdc = 4;
        int TotalNumOlss = vps_max_layers;
        if (vps_max_layers > 1)
        {
            vps_each_layer_is_an_ols_flag = 0;
            if (vps_all_independent_layers_flag)
                vps_each_layer_is_an_ols_flag = m_reader.getBit();
            if (!vps_each_layer_is_an_ols_flag)
            {
                if (!vps_all_independent_layers_flag)
                {
                    vps_ols_mode_idc = m_reader.getBits(2);
                    olsModeIdc = vps_ols_mode_idc;
                }
                if (vps_ols_mode_idc == 2)
                {
                    int vps_num_output_layer_sets_minus2 = m_reader.getBits(8);
                    TotalNumOlss = vps_num_output_layer_sets_minus2 + 2;
                    for (int i = 1; i <= vps_num_output_layer_sets_minus2 + 1; i++)
                        for (int j = 0; j < vps_max_layers; j++) m_reader.skipBit();  // vps_ols_output_layer_flag[i][j]
                }
            }
            vps_num_ptls = m_reader.getBits(8) + 1;
        }

        std::vector<bool> vps_pt_present_flag;
        std::vector<int> vps_ptl_max_tid;
        vps_pt_present_flag.resize(vps_num_ptls);
        vps_ptl_max_tid.resize(vps_num_ptls);

        for (int i = 0; i < vps_num_ptls; i++)
        {
            if (i > 0)
                vps_pt_present_flag[i] = m_reader.getBit();
            if (!vps_default_ptl_dpb_hrd_max_tid_flag)
                vps_ptl_max_tid[i] = m_reader.getBits(3);
        }

        m_reader.skipBits(m_reader.getBitsLeft() % 8);  // vps_ptl_alignment_zero_bit

        for (int i = 0; i < vps_num_ptls; i++)
            if (profile_tier_level(vps_pt_present_flag[i], vps_ptl_max_tid[i]) != 0)
                return 1;

        for (int i = 0; i < TotalNumOlss; i++)
            if (vps_num_ptls > 1 && vps_num_ptls != TotalNumOlss)
                m_reader.skipBits(8);  // vps_ols_ptl_idx[i]

        if (!vps_each_layer_is_an_ols_flag)
        {
            unsigned NumMultiLayerOlss = 0;
            int NumLayersInOls = 0;
            for (int i = 1; i < TotalNumOlss; i++)
            {
                if (vps_each_layer_is_an_ols_flag)
                    NumLayersInOls = 1;
                else if (vps_ols_mode_idc == 0 || vps_ols_mode_idc == 1)
                    NumLayersInOls = i + 1;
                else if (vps_ols_mode_idc == 2)
                {
                    int j = 0;
                    for (int k = 0; k < vps_max_layers; k++) NumLayersInOls = j;
                }
                if (NumLayersInOls > 1)
                    NumMultiLayerOlss++;
            }

            unsigned vps_num_dpb_params = extractUEGolombCode() + 1;
            if (vps_num_dpb_params >= NumMultiLayerOlss)
                return 1;
            unsigned VpsNumDpbParams;
            if (vps_each_layer_is_an_ols_flag)
                VpsNumDpbParams = 0;
            else
                VpsNumDpbParams = vps_num_dpb_params;

            bool vps_sublayer_dpb_params_present_flag =
                (vps_max_sublayers > 1) ? vps_sublayer_dpb_params_present_flag = m_reader.getBit() : 0;

            for (size_t i = 0; i < VpsNumDpbParams; i++)
            {
                int vps_dpb_max_tid = vps_max_sublayers - 1;
                if (!vps_default_ptl_dpb_hrd_max_tid_flag)
                    vps_dpb_max_tid = m_reader.getBits(3);
                if (dpb_parameters(vps_dpb_max_tid, vps_sublayer_dpb_params_present_flag))
                    return 1;
            }

            for (size_t i = 0; i < NumMultiLayerOlss; i++)
            {
                extractUEGolombCode();  // vps_ols_dpb_pic_width
                extractUEGolombCode();  // vps_ols_dpb_pic_height
                m_reader.skipBits(2);   // vps_ols_dpb_chroma_format
                unsigned vps_ols_dpb_bitdepth_minus8 = extractUEGolombCode();
                if (vps_ols_dpb_bitdepth_minus8 > 2)
                    return 1;
                if (VpsNumDpbParams > 1 && VpsNumDpbParams != NumMultiLayerOlss)
                {
                    unsigned vps_ols_dpb_params_idx = extractUEGolombCode();
                    if (vps_ols_dpb_params_idx >= VpsNumDpbParams)
                        return 1;
                }
            }
            bool vps_timing_hrd_params_present_flag = m_reader.getBit();
            if (vps_timing_hrd_params_present_flag)
            {
                if (m_vps_hrd.general_timing_hrd_parameters())
                    return 1;
                bool vps_sublayer_cpb_params_present_flag = (vps_max_sublayers > 1) ? m_reader.getBit() : 0;
                unsigned vps_num_ols_timing_hrd_params = extractUEGolombCode() + 1;
                if (vps_num_ols_timing_hrd_params > NumMultiLayerOlss)
                    return 1;
                for (size_t i = 0; i <= vps_num_ols_timing_hrd_params; i++)
                {
                    int vps_hrd_max_tid = 1;
                    if (!vps_default_ptl_dpb_hrd_max_tid_flag)
                        vps_hrd_max_tid = m_reader.getBits(3);
                    int firstSubLayer = vps_sublayer_cpb_params_present_flag ? 0 : vps_hrd_max_tid;
                    m_vps_hrd.ols_timing_hrd_parameters(firstSubLayer, vps_hrd_max_tid);
                }
                if (vps_num_ols_timing_hrd_params > 1 && vps_num_ols_timing_hrd_params != NumMultiLayerOlss)
                {
                    for (size_t i = 0; i < NumMultiLayerOlss; i++)
                    {
                        unsigned vps_ols_timing_hrd_idx = extractUEGolombCode();
                        if (vps_ols_timing_hrd_idx >= vps_num_ols_timing_hrd_params)
                            return 1;
                    }
                }
            }
        }

        return rez;
    }
    catch (VodCoreException& e)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

void VvcVpsUnit::setFPS(double fps)
{
    time_scale = (uint32_t)(fps + 0.5) * 1000000;
    num_units_in_tick = time_scale / fps + 0.5;

    // num_units_in_tick = time_scale/2 / fps;
    assert(num_units_in_tick_bit_pos > 0);
    updateBits(num_units_in_tick_bit_pos, 32, num_units_in_tick);
    updateBits(num_units_in_tick_bit_pos + 32, 32, time_scale);
}

double VvcVpsUnit::getFPS() const { return num_units_in_tick ? time_scale / (float)num_units_in_tick : 0; }

string VvcVpsUnit::getDescription() const
{
    string rez("Frame rate: ");
    double fps = getFPS();
    if (fps != 0)
        rez += doubleToStr(fps);
    else
        rez += string("not found");

    return rez;
}

// ------------------------- VvcSpsUnit ------------------------------

VvcSpsUnit::VvcSpsUnit()
    : VvcUnitWithProfile(),
      sps_id(0),
      vps_id(0),
      max_sublayers_minus1(0),
      chroma_format_idc(0),
      pic_width_max_in_luma_samples(0),
      pic_height_max_in_luma_samples(0),
      bitdepth_minus8(0),
      log2_max_pic_order_cnt_lsb(0),
      colour_primaries(2),
      transfer_characteristics(2),
      matrix_coeffs(2),  // 2 = unspecified
      full_range_flag(0),
      chroma_sample_loc_type_frame(0),
      chroma_sample_loc_type_top_field(0),
      chroma_sample_loc_type_bottom_field(0),
      num_units_in_tick(0),
      time_scale(0),
      inter_layer_prediction_enabled_flag(0),
      long_term_ref_pics_flag(0),
      sps_num_ref_pic_lists(0),
      weighted_pred_flag(0),
      weighted_bipred_flag(0)
{
}

int VvcSpsUnit::deserialize()
{
    int rez = VvcUnit::deserialize();
    if (rez)
        return rez;
    try
    {
        sps_id = m_reader.getBits(4);
        vps_id = m_reader.getBits(4);
        max_sublayers_minus1 = m_reader.getBits(3);
        if (max_sublayers_minus1 == 7)
            return 1;
        chroma_format_idc = m_reader.getBits(2);
        unsigned sps_log2_ctu_size_minus5 = m_reader.getBits(2);
        if (sps_log2_ctu_size_minus5 > 2)
            return 1;
        int CtbLog2SizeY = sps_log2_ctu_size_minus5 + 5;
        unsigned CtbSizeY = 1 << CtbLog2SizeY;
        bool sps_ptl_dpb_hrd_params_present_flag = m_reader.getBit();
        if (sps_id == 0 && !sps_ptl_dpb_hrd_params_present_flag)
            return 1;
        if (sps_ptl_dpb_hrd_params_present_flag)
            if (profile_tier_level(1, max_sublayers_minus1) != 0)
                return 1;
        m_reader.skipBit();      // sps_gdr_enabled_flag
        if (m_reader.getBit())   // sps_ref_pic_resampling_enabled_flag
            m_reader.skipBit();  // sps_res_change_in_clvs_allowed_flag
        pic_width_max_in_luma_samples = extractUEGolombCode();
        pic_height_max_in_luma_samples = extractUEGolombCode();
        unsigned tmpWidthVal = (pic_width_max_in_luma_samples + CtbSizeY - 1) / CtbSizeY;
        unsigned tmpHeightVal = (pic_height_max_in_luma_samples + CtbSizeY - 1) / CtbSizeY;
        unsigned sps_conf_win_left_offset = 0;
        unsigned sps_conf_win_right_offset = 0;
        unsigned sps_conf_win_top_offset = 0;
        unsigned sps_conf_win_bottom_offset = 0;
        if (m_reader.getBit())  // sps_conformance_window_flag
        {
            sps_conf_win_left_offset = extractUEGolombCode();
            sps_conf_win_right_offset = extractUEGolombCode();
            sps_conf_win_top_offset = extractUEGolombCode();
            sps_conf_win_bottom_offset = extractUEGolombCode();
        }
        if (m_reader.getBit())  // sps_subpic_info_present_flag
        {
            unsigned sps_num_subpics_minus1 = extractUEGolombCode();
            if (sps_num_subpics_minus1 > 600)
                return 1;
            if (sps_num_subpics_minus1 > 0)
            {
                bool sps_independent_subpics_flag = m_reader.getBit();
                bool sps_subpic_same_size_flag = m_reader.getBit();
                for (size_t i = 0; i <= sps_num_subpics_minus1; i++)
                {
                    if (!sps_subpic_same_size_flag || i == 0)
                    {
                        if (i != 0 && pic_width_max_in_luma_samples > CtbSizeY)
                            m_reader.skipBits(ceil(log2(tmpWidthVal)));  // sps_subpic_ctu_top_left_x[i]
                        if (i != 0 && pic_height_max_in_luma_samples > CtbSizeY)
                            m_reader.skipBits(ceil(log2(tmpHeightVal)));  // sps_subpic_ctu_top_left_y[i]
                        if (i < sps_num_subpics_minus1 && pic_width_max_in_luma_samples > CtbSizeY)
                            m_reader.skipBits(ceil(log2(tmpWidthVal)));  // sps_subpic_width_minus1[i]
                        if (i < sps_num_subpics_minus1 && pic_height_max_in_luma_samples > CtbSizeY)
                            m_reader.skipBits(ceil(log2(tmpHeightVal)));  // sps_subpic_height_minus1[i]
                    }
                    if (!sps_independent_subpics_flag)
                    {
                        m_reader.skipBit();  // sps_subpic_treated_as_pic_flag
                        m_reader.skipBit();  // sps_loop_filter_across_subpic_enabled_flag
                    }
                }
            }
            unsigned sps_subpic_id_len = extractUEGolombCode() + 1;
            if (sps_subpic_id_len > 16 || (unsigned)(1 << sps_subpic_id_len) < (sps_num_subpics_minus1 + 1))
                return 1;
            if (m_reader.getBit())  // sps_subpic_id_mapping_explicitly_signalled_flag
            {
                if (m_reader.getBit())  // sps_subpic_id_mapping_present_flag
                    for (size_t i = 0; i <= sps_num_subpics_minus1; i++)
                        m_reader.skipBits(sps_subpic_id_len);  // sps_subpic_id[i]
            }
        }
        bitdepth_minus8 = extractUEGolombCode();
        if (bitdepth_minus8 > 2)
            return 1;
        int QpBdOffset = 6 * bitdepth_minus8;
        m_reader.skipBit();  // sps_entropy_coding_sync_enabled_flag
        m_reader.skipBit();  // vsps_entry_point_offsets_present_flag
        log2_max_pic_order_cnt_lsb = m_reader.getBits(4) + 4;
        if (log2_max_pic_order_cnt_lsb > 16)
            return 1;
        if (m_reader.getBit())  // sps_poc_msb_cycle_flag
        {
            if (extractUEGolombCode() /* sps_poc_msb_cycle_len_minus1 */ > 23 - log2_max_pic_order_cnt_lsb)
                return 1;
        }
        int sps_num_extra_ph_bytes = m_reader.getBits(2);
        for (size_t i = 0; i < sps_num_extra_ph_bytes; i++) m_reader.skipBits(8);  // sps_extra_ph_bit_present_flag[i]
        int sps_num_extra_sh_bytes = m_reader.getBits(2);
        for (size_t i = 0; i < sps_num_extra_sh_bytes; i++) m_reader.skipBits(8);  // sps_extra_sh_bit_present_flag[i]
        if (sps_ptl_dpb_hrd_params_present_flag)
        {
            bool sps_sublayer_dpb_params_flag = (max_sublayers_minus1 > 0) ? m_reader.getBit() : 0;
            if (dpb_parameters(max_sublayers_minus1, sps_sublayer_dpb_params_flag))
                return 1;
        }
        unsigned sps_log2_min_luma_coding_block_size_minus2 = extractUEGolombCode();
        if (sps_log2_min_luma_coding_block_size_minus2 > (unsigned)min(4, (int)sps_log2_ctu_size_minus5 + 3))
            return 1;
        unsigned MinCbLog2SizeY = sps_log2_min_luma_coding_block_size_minus2 + 2;
        unsigned MinCbSizeY = 1 << MinCbLog2SizeY;
        m_reader.skipBit();  // sps_partition_constraints_override_enabled_flag
        unsigned sps_log2_diff_min_qt_min_cb_intra_slice_luma = extractUEGolombCode();
        if (sps_log2_diff_min_qt_min_cb_intra_slice_luma > min(6, CtbLog2SizeY) - MinCbLog2SizeY)
            return 1;
        unsigned MinQtLog2SizeIntraY = sps_log2_diff_min_qt_min_cb_intra_slice_luma + MinCbLog2SizeY;
        unsigned sps_max_mtt_hierarchy_depth_intra_slice_luma = extractUEGolombCode();
        if (sps_max_mtt_hierarchy_depth_intra_slice_luma > 2 * (CtbLog2SizeY - MinCbLog2SizeY))
            return 1;
        if (sps_max_mtt_hierarchy_depth_intra_slice_luma != 0)
        {
            if (extractUEGolombCode() >
                CtbLog2SizeY - MinQtLog2SizeIntraY)  // sps_log2_diff_max_bt_min_qt_intra_slice_luma
                return 1;
            if (extractUEGolombCode() >
                min(6, CtbLog2SizeY) - MinQtLog2SizeIntraY)  // sps_log2_diff_max_tt_min_qt_intra_slice_luma
                return 1;
        }
        bool sps_qtbtt_dual_tree_intra_flag = (chroma_format_idc != 0 ? m_reader.getBit() : 0);
        if (sps_qtbtt_dual_tree_intra_flag)
        {
            unsigned sps_log2_diff_min_qt_min_cb_intra_slice_chroma = extractUEGolombCode();
            if (sps_log2_diff_min_qt_min_cb_intra_slice_chroma > min(6, CtbLog2SizeY) - MinCbLog2SizeY)  //
                return 1;
            unsigned MinQtLog2SizeIntraC = sps_log2_diff_min_qt_min_cb_intra_slice_chroma + MinCbLog2SizeY;
            unsigned sps_max_mtt_hierarchy_depth_intra_slice_chroma = extractUEGolombCode();
            if (sps_max_mtt_hierarchy_depth_intra_slice_chroma > 2 * (CtbLog2SizeY - MinCbLog2SizeY))
                return 1;
            if (sps_max_mtt_hierarchy_depth_intra_slice_chroma != 0)
            {
                if (extractUEGolombCode() >
                    min(6, CtbLog2SizeY) - MinQtLog2SizeIntraC)  // sps_log2_diff_max_bt_min_qt_intra_slice_chroma
                    return 1;
                if (extractUEGolombCode() >
                    min(6, CtbLog2SizeY) - MinQtLog2SizeIntraC)  // sps_log2_diff_max_tt_min_qt_intra_slice_chroma
                    return 1;
            }
        }
        unsigned sps_log2_diff_min_qt_min_cb_inter_slice = extractUEGolombCode();
        if (sps_log2_diff_min_qt_min_cb_inter_slice > min(6, CtbLog2SizeY) - MinCbLog2SizeY)
            return 1;
        unsigned MinQtLog2SizeInterY = sps_log2_diff_min_qt_min_cb_inter_slice + MinCbLog2SizeY;
        unsigned sps_max_mtt_hierarchy_depth_inter_slice = extractUEGolombCode();
        if (sps_max_mtt_hierarchy_depth_inter_slice > 2 * (CtbLog2SizeY - MinCbLog2SizeY))
            return 1;
        if (sps_max_mtt_hierarchy_depth_inter_slice != 0)
        {
            if (extractUEGolombCode() > CtbLog2SizeY - MinQtLog2SizeInterY)  // sps_log2_diff_max_bt_min_qt_inter_slice
                return 1;
            if (extractUEGolombCode() >
                min(6, CtbLog2SizeY) - MinQtLog2SizeInterY)  // sps_log2_diff_max_tt_min_qt_inter_slice
                return 1;
        }
        bool sps_max_luma_transform_size_64_flag = (CtbSizeY > 32 ? m_reader.getBit() : 0);
        bool sps_transform_skip_enabled_flag = m_reader.getBit();
        if (sps_transform_skip_enabled_flag)
        {
            if (extractUEGolombCode() > 3)  // sps_log2_transform_skip_max_size_minus2
                return 1;
            m_reader.skipBit();  // sps_bdpcm_enabled_flag
        }
        if (m_reader.getBit())  // sps_mts_enabled_flag
        {
            m_reader.skipBit();  // sps_explicit_mts_intra_enabled_flag
            m_reader.skipBit();  // sps_explicit_mts_inter_enabled_flag
        }
        bool sps_lfnst_enabled_flag = m_reader.getBit();
        if (chroma_format_idc != 0)
        {
            bool sps_joint_cbcr_enabled_flag = m_reader.getBit();
            int numQpTables =
                m_reader.getBit() /* sps_same_qp_table_for_chroma_flag */ ? 1 : (sps_joint_cbcr_enabled_flag ? 3 : 2);
            for (int i = 0; i < numQpTables; i++)
            {
                int sps_qp_table_start_minus26 = extractSEGolombCode();
                if (sps_qp_table_start_minus26 < (-26 - QpBdOffset) || sps_qp_table_start_minus26 > 36)
                    return 1;
                unsigned sps_num_points_in_qp_table_minus1 = extractUEGolombCode();
                if (sps_num_points_in_qp_table_minus1 > (unsigned)(36 - sps_qp_table_start_minus26))
                    return 1;
                for (size_t j = 0; j <= sps_num_points_in_qp_table_minus1; j++)
                {
                    extractUEGolombCode();  // sps_delta_qp_in_val_minus1
                    extractUEGolombCode();  // sps_delta_qp_diff_val
                }
            }
        }
        m_reader.skipBit();  // sps_sao_enabled_flag
        if (m_reader.getBit() /* sps_alf_enabled_flag */ && chroma_format_idc != 0)
            m_reader.skipBit();  // sps_ccalf_enabled_flag
        m_reader.skipBit();      // sps_lmcs_enabled_flag
        weighted_pred_flag = m_reader.getBit();
        weighted_bipred_flag = m_reader.getBit();
        long_term_ref_pics_flag = m_reader.getBit();
        inter_layer_prediction_enabled_flag = (sps_id != 0) ? m_reader.getBit() : 0;
        m_reader.skipBit();  // sps_idr_rpl_present_flag
        bool sps_rpl1_same_as_rpl0_flag = m_reader.getBit();
        for (size_t i = 0; i < (sps_rpl1_same_as_rpl0_flag ? 1 : 2); i++)
        {
            sps_num_ref_pic_lists = extractUEGolombCode();
            if (sps_num_ref_pic_lists > 64)
                return 1;
            for (size_t j = 0; j < sps_num_ref_pic_lists; j++) ref_pic_list_struct(i, j);
        }
        m_reader.skipBit();  // sps_ref_wraparound_enabled_flag
        bool sps_sbtmvp_enabled_flag = (m_reader.getBit()) /* sps_temporal_mvp_enabled_flag */ ? m_reader.getBit() : 0;
        bool sps_amvr_enabled_flag = m_reader.getBit();
        if (m_reader.getBit())   // sps_bdof_enabled_flag
            m_reader.skipBit();  // sps_bdof_control_present_in_ph_flag
        m_reader.skipBit();      // sps_smvd_enabled_flag
        if (m_reader.getBit())   // sps_dmvr_enabled_flag
            m_reader.skipBit();  // sps_dmvr_control_present_in_ph_flag
        if (m_reader.getBit())   // sps_mmvd_enabled_flag
            m_reader.skipBit();  // sps_mmvd_fullpel_only_enabled_flag
        unsigned sps_six_minus_max_num_merge_cand = extractUEGolombCode();
        if (sps_six_minus_max_num_merge_cand > 5)
            return 1;
        unsigned MaxNumMergeCand = 6 - sps_six_minus_max_num_merge_cand;
        m_reader.skipBit();     // sps_sbt_enabled_flag
        if (m_reader.getBit())  // sps_affine_enabled_flag
        {
            unsigned sps_five_minus_max_num_subblock_merge_cand = extractUEGolombCode();
            if (sps_five_minus_max_num_subblock_merge_cand + sps_sbtmvp_enabled_flag > 5)
                return 1;
            m_reader.skipBit();  // sps_6param_affine_enabled_flag
            if (sps_amvr_enabled_flag)
                m_reader.skipBit();  // sps_affine_amvr_enabled_flag
            if (m_reader.getBit())   // sps_affine_prof_enabled_flag
                m_reader.skipBit();  // sps_prof_control_present_in_ph_flag
        }
        m_reader.skipBit();  // sps_bcw_enabled_flag
        m_reader.skipBit();  // sps_ciip_enabled_flag
        if (MaxNumMergeCand >= 2)
        {
            if (m_reader.getBit() /* sps_gpm_enabled_flag */ && MaxNumMergeCand >= 3)
            {
                unsigned sps_max_num_merge_cand_minus_max_num_gpm_cand = extractUEGolombCode();
                if (sps_max_num_merge_cand_minus_max_num_gpm_cand + 2 > MaxNumMergeCand)
                    return 1;
            }
        }
        unsigned sps_log2_parallel_merge_level_minus2 = extractUEGolombCode();
        if (sps_log2_parallel_merge_level_minus2 > CtbLog2SizeY - 2)
            return 1;

        bool sps_isp_enabled_flag = m_reader.getBit();
        bool sps_mrl_enabled_flag = m_reader.getBit();
        bool sps_mip_enabled_flag = m_reader.getBit();
        if (chroma_format_idc != 0)
            bool sps_cclm_enabled_flag = m_reader.getBit();
        if (chroma_format_idc == 1)
        {
            bool sps_chroma_horizontal_collocated_flag = m_reader.getBit();
            bool sps_chroma_vertical_collocated_flag = m_reader.getBit();
        }
        bool sps_palette_enabled_flag = m_reader.getBit();
        bool sps_act_enabled_flag =
            (chroma_format_idc == 3 && !sps_max_luma_transform_size_64_flag) ? m_reader.getBit() : 0;
        if (sps_transform_skip_enabled_flag || sps_palette_enabled_flag)
        {
            unsigned sps_min_qp_prime_ts = extractUEGolombCode();
            if (sps_min_qp_prime_ts > 8)
                return 1;
        }
        if (m_reader.getBit())  // sps_ibc_enabled_flag
        {
            unsigned sps_six_minus_max_num_ibc_merge_cand = extractUEGolombCode();
            if (sps_six_minus_max_num_ibc_merge_cand > 5)
                return 1;
        }
        if (m_reader.getBit())  // sps_ladf_enabled_flag
        {
            int sps_num_ladf_intervals_minus2 = m_reader.getBits(2);
            int sps_ladf_lowest_interval_qp_offset = extractSEGolombCode();
            for (int i = 0; i < sps_num_ladf_intervals_minus2 + 1; i++)
            {
                int sps_ladf_qp_offset = extractSEGolombCode();
                unsigned sps_ladf_delta_threshold_minus1 = extractUEGolombCode();
            }
        }
        bool sps_explicit_scaling_list_enabled_flag = m_reader.getBit();
        if (sps_lfnst_enabled_flag && sps_explicit_scaling_list_enabled_flag)
            bool sps_scaling_matrix_for_lfnst_disabled_flag = m_reader.getBit();
        bool sps_scaling_matrix_for_alternative_colour_space_disabled_flag =
            (sps_act_enabled_flag && sps_explicit_scaling_list_enabled_flag) ? m_reader.getBit() : 0;
        if (sps_scaling_matrix_for_alternative_colour_space_disabled_flag)
            bool sps_scaling_matrix_designated_colour_space_flag = m_reader.getBit();
        bool sps_dep_quant_enabled_flag = m_reader.getBit();
        bool sps_sign_data_hiding_enabled_flag = m_reader.getBit();
        if (m_reader.getBit())  // sps_virtual_boundaries_enabled_flag
        {
            if (m_reader.getBit())  // sps_virtual_boundaries_present_flag
            {
                unsigned sps_num_ver_virtual_boundaries = extractUEGolombCode();
                if (sps_num_ver_virtual_boundaries > (pic_width_max_in_luma_samples <= 8 ? 0 : 3))
                    return 1;
                for (size_t i = 0; i < sps_num_ver_virtual_boundaries; i++)
                {
                    unsigned sps_virtual_boundary_pos_x_minus1 = extractUEGolombCode();
                    if (sps_virtual_boundary_pos_x_minus1 > ceil(pic_width_max_in_luma_samples / 8) - 2)
                        return 1;
                }
                unsigned sps_num_hor_virtual_boundaries = extractUEGolombCode();
                if (sps_num_hor_virtual_boundaries > (pic_height_max_in_luma_samples <= 8 ? 0 : 3))
                    return 1;
                for (size_t i = 0; i < sps_num_hor_virtual_boundaries; i++)
                {
                    unsigned sps_virtual_boundary_pos_y_minus1 = extractUEGolombCode();
                    if (sps_virtual_boundary_pos_y_minus1 > ceil(pic_height_max_in_luma_samples / 8) - 2)
                        return 1;
                }
            }
        }
        if (sps_ptl_dpb_hrd_params_present_flag)
        {
            if (m_reader.getBit())  // sps_timing_hrd_params_present_flag
            {
                if (m_sps_hrd.general_timing_hrd_parameters())
                    return 1;
                int sps_sublayer_cpb_params_present_flag = (max_sublayers_minus1 > 0) ? m_reader.getBit() : 0;
                int firstSubLayer = sps_sublayer_cpb_params_present_flag ? 0 : max_sublayers_minus1;
                m_sps_hrd.ols_timing_hrd_parameters(firstSubLayer, max_sublayers_minus1);
            }
        }
        bool sps_field_seq_flag = m_reader.getBit();
        if (m_reader.getBit())  // sps_vui_parameters_present_flag
        {
            unsigned sps_vui_payload_size_minus1 = extractUEGolombCode();
            if (sps_vui_payload_size_minus1 > 1023)
                return 1;
            m_reader.skipBits(m_reader.getBitsLeft() % 8);  // sps_vui_alignment_zero_bit
            vui_parameters();
        }
        bool sps_extension_flag = m_reader.getBit();
        return 0;
    }
    catch (VodCoreException& e)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

int VvcSpsUnit::ref_pic_list_struct(int listIdx, int rplsIdx)
{
    unsigned num_ref_entries = extractUEGolombCode();
    bool ltrp_in_header_flag = 1;
    if (long_term_ref_pics_flag && rplsIdx < sps_num_ref_pic_lists && num_ref_entries > 0)
        ltrp_in_header_flag = m_reader.getBit();
    for (size_t i = 0; i < num_ref_entries; i++)
    {
        bool inter_layer_ref_pic_flag = (inter_layer_prediction_enabled_flag) ? m_reader.getBit() : 0;
        if (!inter_layer_ref_pic_flag)
        {
            bool st_ref_pic_flag = (long_term_ref_pics_flag) ? m_reader.getBit() : 1;
            if (st_ref_pic_flag)
            {
                unsigned abs_delta_poc_st = extractUEGolombCode();
                if (abs_delta_poc_st > (2 << 14) - 1)
                    return 1;
                unsigned AbsDeltaPocSt = abs_delta_poc_st + 1;
                if ((weighted_pred_flag || weighted_bipred_flag) && i != 0)
                    AbsDeltaPocSt -= 1;
                if (AbsDeltaPocSt > 0)
                    bool strp_entry_sign_flag = m_reader.getBit();
            }
            else if (!ltrp_in_header_flag)
                int rpls_poc_lsb_lt = m_reader.getBits(log2_max_pic_order_cnt_lsb);
        }
        else
            unsigned ilrp_idx = extractUEGolombCode();
    }
    return 0;
}

double VvcSpsUnit::getFPS() const { return num_units_in_tick ? time_scale / (float)num_units_in_tick : 0; }

string VvcSpsUnit::getDescription() const
{
    string result = getProfileString();
    result += string(" Resolution: ") + int32ToStr(pic_width_max_in_luma_samples) + string(":") +
              int32ToStr(pic_height_max_in_luma_samples) + string("p");

    double fps = getFPS();
    result += "  Frame rate: ";
    result += (fps ? doubleToStr(fps) : string("not found"));
    return result;
}

int VvcSpsUnit::vui_parameters()
{
    bool progressive_source_flag = m_reader.getBit();
    bool interlaced_source_flag = m_reader.getBit();
    m_reader.skipBit();  // non_packed_constraint_flag
    m_reader.skipBit();  // non_projected_constraint_flag

    if (m_reader.getBit())  // aspect_ratio_info_present_flag
    {
        if (m_reader.getBits(8) == EXTENDED_SAR)  // aspect_ratio_idc
            m_reader.skipBits(32);                // sar_width, sar_height
    }

    if (m_reader.getBit())   // overscan_info_present_flag
        m_reader.skipBit();  // overscan_appropriate_flag
    if (m_reader.getBit())   // colour_description_present_flag
    {
        colour_primaries = m_reader.getBits(8);
        transfer_characteristics = m_reader.getBits(8);
        matrix_coeffs = m_reader.getBits(8);
        full_range_flag = m_reader.getBit();
    }

    if (m_reader.getBit())  // chroma_loc_info_present_flag
    {
        if (progressive_source_flag && !interlaced_source_flag)
            chroma_sample_loc_type_frame = extractUEGolombCode();
        else
        {
            chroma_sample_loc_type_top_field = extractUEGolombCode();
            if (chroma_sample_loc_type_top_field > 5)
                return 1;
            chroma_sample_loc_type_bottom_field = extractUEGolombCode();
            if (chroma_sample_loc_type_bottom_field > 5)
                return 1;
        }
    }
    return 0;
}

// ----------------------- VvcPpsUnit ------------------------
VvcPpsUnit::VvcPpsUnit() : pps_id(-1), sps_id(-1) {}

int VvcPpsUnit::deserialize()
{
    int rez = VvcUnit::deserialize();
    if (rez)
        return rez;

    try
    {
        pps_id = m_reader.getBits(6);
        sps_id = m_reader.getBits(4);
        // bool pps_mixed_nalu_types_in_pic_flag= m_reader.getBit();
        // int pps_pic_width_in_luma_samples = extractUEGolombCode();
        // int pps_pic_height_in_luma_samples = extractUEGolombCode();

        return 0;
    }
    catch (VodCoreException& e)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

// ----------------------- VvcHrdUnit ------------------------
VvcHrdUnit::VvcHrdUnit()
    : general_nal_hrd_params_present_flag(0),
      general_du_hrd_params_present_flag(0),
      general_vcl_hrd_params_present_flag(0),
      hrd_cpb_cnt_minus1(0),
      num_units_in_tick(0),
      time_scale(0)
{
}

bool VvcHrdUnit::general_timing_hrd_parameters()
{
    num_units_in_tick = m_reader.getBits(32);
    time_scale = m_reader.getBits(32);
    general_nal_hrd_params_present_flag = m_reader.getBit();
    general_vcl_hrd_params_present_flag = m_reader.getBit();
    if (general_nal_hrd_params_present_flag || general_vcl_hrd_params_present_flag)
    {
        m_reader.skipBit();  // general_same_pic_timing_in_all_ols_flag
        general_du_hrd_params_present_flag = m_reader.getBit();
        if (general_du_hrd_params_present_flag)
            int tick_divisor_minus2 = m_reader.getBits(8);
        m_reader.skipBits(4);  // bit_rate_scale
        m_reader.skipBits(4);  // cpb_size_scale
        if (general_du_hrd_params_present_flag)
            m_reader.skipBits(4);  // cpb_size_du_scale
        hrd_cpb_cnt_minus1 = extractUEGolombCode();
        if (hrd_cpb_cnt_minus1 > 31)
            return 1;
    }
    return 0;
}

bool VvcHrdUnit::ols_timing_hrd_parameters(int firstSubLayer, int MaxSubLayersVal)
{
    for (int i = firstSubLayer; i <= MaxSubLayersVal; i++)
    {
        bool fixed_pic_rate_within_cvs_flag =
            m_reader.getBit() /* fixed_pic_rate_general_flag) */ ? 1 : m_reader.getBit();
        if (fixed_pic_rate_within_cvs_flag)
        {
            if (extractUEGolombCode() > 2047)  // elemental_duration_in_tc_minus1
                return 1;
        }
        else if ((general_nal_hrd_params_present_flag || general_vcl_hrd_params_present_flag) &&
                 hrd_cpb_cnt_minus1 == 0)
            m_reader.skipBit();  // low_delay_hrd_flag
        if (general_nal_hrd_params_present_flag)
            sublayer_hrd_parameters(i);
        if (general_vcl_hrd_params_present_flag)
            sublayer_hrd_parameters(i);
    }
    return 0;
}

bool VvcHrdUnit::sublayer_hrd_parameters(int subLayerId)
{
    for (int j = 0; j <= hrd_cpb_cnt_minus1; j++)
    {
        unsigned bit_rate_value_minus1 = extractUEGolombCode();
        if (bit_rate_value_minus1 == 0xffffffff)
            return 1;
        unsigned cpb_size_value_minus1 = extractUEGolombCode();
        if (cpb_size_value_minus1 == 0xffffffff)
            return 1;
        if (general_du_hrd_params_present_flag)
        {
            unsigned cpb_size_du_value_minus1 = extractUEGolombCode();
            if (cpb_size_du_value_minus1 == 0xffffffff)
                return 1;
            unsigned bit_rate_du_value_minus1 = extractUEGolombCode();
            if (bit_rate_du_value_minus1 == 0xffffffff)
                return 1;
        }
        m_reader.skipBit();  // cbr_flag
    }
    return 0;
}

// -----------------------  VvcSliceHeader() -------------------------------------

VvcSliceHeader::VvcSliceHeader() : VvcUnit(), ph_pps_id(-1), pic_order_cnt_lsb(0) {}

int VvcSliceHeader::deserialize(const VvcSpsUnit* sps, const VvcPpsUnit* pps)
{
    int rez = VvcUnit::deserialize();
    if (rez)
        return rez;

    try
    {
        if (m_reader.getBit())  // sh_picture_header_in_slice_header_flag
        {
            bool ph_gdr_or_irap_pic_flag = m_reader.getBit();
            m_reader.skipBit();  // ph_non_ref_pic_flag
            if (ph_gdr_or_irap_pic_flag)
                m_reader.skipBit();  // ph_gdr_pic_flag
            if (m_reader.getBit())   // ph_inter_slice_allowed_flag
                m_reader.skipBit();  // ph_intra_slice_allowed_flag
            unsigned ph_pps_id = extractUEGolombCode();
            if (ph_pps_id > 63)
                return 1;
            pic_order_cnt_lsb = m_reader.getBits(sps->log2_max_pic_order_cnt_lsb);
            ;
        }

        return 0;
    }
    catch (VodCoreException& e)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

bool VvcSliceHeader::isIDR() const { return nal_unit_type == V_IDR_W_RADL || nal_unit_type == V_IDR_N_LP; }

vector<vector<uint8_t>> vvc_extract_priv_data(const uint8_t* buff, int size, int* nal_size)
{
    *nal_size = 4;

    vector<vector<uint8_t>> spsPps;
    if (size < 23)
        return spsPps;

    *nal_size = (buff[21] & 3) + 1;
    int num_arrays = buff[22];

    const uint8_t* src = buff + 23;
    const uint8_t* end = buff + size;
    for (int i = 0; i < num_arrays; ++i)
    {
        if (src + 3 > end)
            THROW(ERR_MOV_PARSE, "Invalid VVC extra data format");
        int type = *src++;
        int cnt = AV_RB16(src);
        src += 2;

        for (int j = 0; j < cnt; ++j)
        {
            if (src + 2 > end)
                THROW(ERR_MOV_PARSE, "Invalid VVC extra data format");
            int nalSize = (src[0] << 8) + src[1];
            src += 2;
            if (src + nalSize > end)
                THROW(ERR_MOV_PARSE, "Invalid VVC extra data format");
            if (nalSize > 0)
            {
                spsPps.push_back(vector<uint8_t>());
                for (int i = 0; i < nalSize; ++i, ++src) spsPps.rbegin()->push_back(*src);
            }
        }
    }

    return spsPps;
}
