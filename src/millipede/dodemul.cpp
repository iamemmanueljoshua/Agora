/**
 * Author: Jian Ding
 * Email: jianding17@gmail.com
 * 
 */
#include "dodemul.hpp"
#include "Consumer.hpp"

using namespace arma;
DoDemul::DoDemul(Config* cfg, int in_tid,
    Consumer& in_consumer,
    Table<complex_float>& in_data_buffer, Table<complex_float>& in_precoder_buffer,
    Table<complex_float>& in_equal_buffer, Table<uint8_t>& in_demod_hard_buffer,
    Table<int8_t>& in_demod_soft_buffer, Stats* in_stats_manager)
    : consumer_(in_consumer)
    , data_buffer_(in_data_buffer)
    , precoder_buffer_(in_precoder_buffer)
    , equal_buffer_(in_equal_buffer)
    , demod_hard_buffer_(in_demod_hard_buffer)
    , demod_soft_buffer_(in_demod_soft_buffer)
    , Demul_task_duration(in_stats_manager->demul_stats_worker.task_duration)
{
    config_ = cfg;
    BS_ANT_NUM = cfg->BS_ANT_NUM;
    UE_NUM = cfg->UE_NUM;
    OFDM_DATA_NUM = cfg->OFDM_DATA_NUM;

    tid = in_tid;

    Demul_task_count = in_stats_manager->demul_stats_worker.task_count;
    // Demul_task_duration = in_Demul_task_duration;
    // Demul_task_count = in_Demul_task_count;

    int demul_block_size = config_->demul_block_size;
    spm_buffer = (complex_float*)aligned_alloc(64, 8 * BS_ANT_NUM * sizeof(complex_float));
    equaled_buffer_temp = (complex_float*)aligned_alloc(64, demul_block_size * UE_NUM * sizeof(complex_float));
    equaled_buffer_temp_transposed = (complex_float*)aligned_alloc(64, demul_block_size * UE_NUM * sizeof(complex_float));

    ue_num_simd256 = UE_NUM / double_num_in_simd256;
}

DoDemul::~DoDemul()
{
    free(spm_buffer);
    free(equaled_buffer_temp);
}

void DoDemul::Demul(int offset)
{

    int frame_id, total_data_subframe_id, current_data_subframe_id, sc_id;
    interpreteOffset3d(offset, &frame_id, &current_data_subframe_id, &sc_id);
    total_data_subframe_id = current_data_subframe_id + frame_id * config_->data_symbol_num_perframe;

#if DEBUG_UPDATE_STATS
    double start_time = get_time();
#endif

#if DEBUG_PRINT_IN_TASK
    printf("In doDemul thread %d: frame: %d, subframe: %d, subcarrier: %d \n", tid, frame_id, current_data_subframe_id, sc_id);
#endif

    int transpose_block_size = config_->transpose_block_size;
    __m256i index = _mm256_setr_epi32(0, 1, transpose_block_size * 2, transpose_block_size * 2 + 1, transpose_block_size * 4,
        transpose_block_size * 4 + 1, transpose_block_size * 6, transpose_block_size * 6 + 1);
    int gather_step_size = 8 * transpose_block_size;

    // int mat_elem = UE_NUM * BS_ANT_NUM;
    // int cache_line_num = mat_elem / 8;
    // int ue_data_cache_line_num = UE_NUM/8;
    int demul_block_size = config_->demul_block_size;
    int max_sc_ite = std::min(demul_block_size, OFDM_DATA_NUM - sc_id);

    /* i = 0, 1, ..., 32/8
     * iterate through cache lines (each cache line has 8 subcarriers) */
    for (int i = 0; i < max_sc_ite / 8; i++) {
/* for a cache line size of 64 bytes, each read load 8 subcarriers
         * use spatial locality to reduce latency */
#if DEBUG_UPDATE_STATS_DETAILED
        double start_time1 = get_time();
#endif

        // for (int j = 0; j < 1; j++) {
        //     int cur_sc_id = i * 8 + j + sc_id;
        //     int precoder_offset = frame_id * OFDM_DATA_NUM + cur_sc_id;
        //     cx_float* precoder_ptr = (cx_float *)precoder_buffer_.precoder[precoder_offset];
        //     for (int line_idx = 0; line_idx < cache_line_num; line_idx ++) {
        //         _mm_prefetch((char *)(precoder_ptr + 8 * line_idx), _MM_HINT_T2);
        //     }
        // }

        int cur_block_id = (sc_id + i * 8) / transpose_block_size;
        int sc_inblock_idx = (i * 8) % transpose_block_size;
        int cur_sc_offset = cur_block_id * transpose_block_size * BS_ANT_NUM + sc_inblock_idx;
        float* tar_data_ptr = (float*)spm_buffer;
        float* src_data_ptr = (float*)data_buffer_[total_data_subframe_id] + cur_sc_offset * 2;

        /* gather data for all antennas and 8 subcarriers in the same cache line */
        for (int ant_idx = 0; ant_idx < BS_ANT_NUM; ant_idx += 4) {
            /* 1 subcarrier and 4 ants per iteration */
            for (int j = 0; j < 8; j++) {
                __m256 data_rx = _mm256_i32gather_ps(src_data_ptr + j * 2, index, 4);
                _mm256_store_ps(tar_data_ptr + j * BS_ANT_NUM * 2, data_rx);
                // printf("Frame %d, sc: %d, UE %d, ant %d, data: %.4f, %.4f, %.4f, %.4f, %.4f, %.4f\n", frame_id, sc_id, ue_idx, ant_idx, *((float *)tar_csi_ptr), *((float *)tar_csi_ptr+1),
                //         *((float *)tar_csi_ptr+2), *((float *)tar_csi_ptr+3),  *((float *)tar_csi_ptr+4), *((float *)tar_csi_ptr+5));
            }
            src_data_ptr += gather_step_size;
            tar_data_ptr += 8;
        }
#if DEBUG_UPDATE_STATS_DETAILED
        double duration1 = get_time() - start_time1;
        Demul_task_duration[tid * 8][1] += duration1;
#endif

        /* perform computation for 8 subcarriers */
        for (int j = 0; j < 8; j++) {
            /* create input data matrix */
            cx_float* data_ptr = (cx_float*)(spm_buffer + j * BS_ANT_NUM);
            cx_fmat mat_data(data_ptr, BS_ANT_NUM, 1, false);

            /* create input precoder matrix */
            int cur_sc_id = i * 8 + j + sc_id;
            int precoder_offset = frame_id * OFDM_DATA_NUM + cur_sc_id;
            if (config_->freq_orthogonal_pilot)
                precoder_offset = precoder_offset - cur_sc_id % UE_NUM;
            cx_float* precoder_ptr = (cx_float*)precoder_buffer_[precoder_offset];
            cx_fmat mat_precoder(precoder_ptr, UE_NUM, BS_ANT_NUM, false);
// cout<<"Precoder: "<< mat_precoder<<endl;

/* create output matrix for equalization */
#if EXPORT_CONSTELLATION
            cx_float* equal_ptr = (cx_float*)(&equal_buffer_[total_data_subframe_id][cur_sc_id * UE_NUM]);
#else
            cx_float* equal_ptr = (cx_float*)(&equaled_buffer_temp[(cur_sc_id - sc_id) * UE_NUM]);
#endif
            cx_fmat mat_equaled(equal_ptr, UE_NUM, 1, false);

#if DEBUG_UPDATE_STATS_DETAILED
            double start_time2 = get_time();
#endif
            /* perform computation for equalization */
            mat_equaled = mat_precoder * mat_data;
            // cout<<mat_equaled.st()<<endl;

#if DEBUG_UPDATE_STATS_DETAILED
            double start_time3 = get_time();
            double duration2 = get_time() - start_time2;
            Demul_task_duration[tid * 8][2] += duration2;
#endif
            // printf("In doDemul thread %d: frame: %d, subframe: %d, subcarrier: %d, sc_id: %d \n", tid, frame_id, current_data_subframe_id,cur_sc_id, sc_id);
            // cout << "Equaled data sc "<<cur_sc_id<<": "<<mat_equaled.st()<<endl;

#ifndef USE_LDPC
            /* decode with hard decision */
            uint8_t* demul_ptr = (&demod_hard_buffer_[total_data_subframe_id][cur_sc_id * UE_NUM]);
            demod_16qam_hard_avx2((float*)equal_ptr, demul_ptr, UE_NUM);
// demod_16qam_hard_loop((float *)equal_ptr, demul_ptr, UE_NUM);
#if DEBUG_UPDATE_STATS_DETAILED
            double duration3 = get_time() - start_time3;
            Demul_task_duration[tid * 8][3] += duration3;
#endif

            // printf("In doDemul thread %d: frame: %d, subframe: %d, subcarrier: %d, sc_id: %d \n", tid, frame_id, current_data_subframe_id,cur_sc_id, sc_id);
            // cout<< "Demuled data: ";
            // for (int ue_idx = 0; ue_idx < UE_NUM; ue_idx++) {
            //     // cout<<demod_hard_buffer_[total_data_subframe_id][cur_sc_id * UE_NUM+ue_idx]<<" "<<endl;
            //     cout<<+*(demul_ptr+ue_idx)<<"  ";
            // }
            // cout<<endl;
#endif

#if DEBUG_UPDATE_STATS
            Demul_task_count[tid * 16] = Demul_task_count[tid * 16] + 1;
#endif
        }
    }

    // #if !ENABLE_DECODE
    // // printf("In doDemul thread %d: frame: %d, subframe: %d, sc_id: %d \n", tid, frame_id, current_data_subframe_id, sc_id);
    // // cout<< "Demuled data: \n";
    // __m256i index2 = _mm256_setr_epi32(0, 1, UE_NUM * 2, UE_NUM * 2 + 1, UE_NUM * 4,
    //                                     UE_NUM* 4 + 1, UE_NUM * 6, UE_NUM* 6 + 1);
    // for (int i = 0; i < UE_NUM; i++) {
    //     float* equal_ptr = (float *)(equaled_buffer_temp + i);
    //     uint8_t *demul_ptr = (&demod_hard_buffer_[total_data_subframe_id][OFDM_DATA_NUM * i + sc_id]);
    //     // cout<<"UE "<<i<<": ";
    //     for (int j = 0; j < max_sc_ite / double_num_in_simd256; j++) {
    //         __m256 equal_T_temp = _mm256_i32gather_ps(equal_ptr, index2, 4);
    //         demod_16qam_loop_simd((float *)&equal_T_temp, demul_ptr, double_num_in_simd256, 1);

    //         // for (int k = 0; k < double_num_in_simd256; k++) {
    //         //     // printf("(%.3f,%.3f) ", ((float *)&equal_T_temp)[k * 2], ((float *)&equal_T_temp)[k * 2 + 1]);
    //         //     printf("%i ", *(demul_ptr + k));;
    //         // }
    //         equal_ptr += UE_NUM * double_num_in_simd256 * 2;
    //         demul_ptr += double_num_in_simd256;
    //     }
    //     // cout<<endl;
    // }
    // #endif

#ifdef USE_LDPC
    // printf("In doDemul thread %d: frame: %d, subframe: %d, sc_id: %d \n", tid, frame_id, current_data_subframe_id, sc_id);
    // cout<< "Demuled data: \n";
    __m256i index2 = _mm256_setr_epi32(0, 1, UE_NUM * 2, UE_NUM * 2 + 1, UE_NUM * 4,
        UE_NUM * 4 + 1, UE_NUM * 6, UE_NUM * 6 + 1);
    float* equal_T_ptr = (float*)(equaled_buffer_temp_transposed);
    for (int i = 0; i < UE_NUM; i++) {
        float* equal_ptr = (float*)(equaled_buffer_temp + i);
        int8_t* demul_ptr = (&demod_soft_buffer_[total_data_subframe_id][(OFDM_DATA_NUM * i + sc_id) * config_->mod_type]);
        for (int j = 0; j < max_sc_ite / double_num_in_simd256; j++) {
            __m256 equal_T_temp = _mm256_i32gather_ps(equal_ptr, index2, 4);
            _mm256_store_ps(equal_T_ptr, equal_T_temp);
            equal_T_ptr += 8;
            equal_ptr += UE_NUM * double_num_in_simd256 * 2;
        }
        int num_sc_avx2 = (max_sc_ite / 16) * 16;
        int rest = max_sc_ite % 16;
        // demod_16qam_soft_sse((equal_T_ptr - max_sc_ite * 2), demul_ptr, max_sc_ite);
        demod_16qam_soft_avx2((equal_T_ptr - max_sc_ite * 2), demul_ptr, num_sc_avx2);
        if (rest > 0)
            demod_16qam_soft_sse((equal_T_ptr - max_sc_ite * 2 + num_sc_avx2 * 2), demul_ptr + config_->mod_type * num_sc_avx2, rest);

        // cout<<"UE "<<i<<": ";
        // for (int k = 0; k < max_sc_ite * config_->mod_order; k++)
        //     printf("%i ", demul_ptr[k]);
        // cout<<endl;
    }
#endif

#if DEBUG_UPDATE_STATS
    double duration = get_time() - start_time;
    Demul_task_duration[tid * 8][0] += duration;
    if (duration > 500) {
        printf("Thread %d Demul takes %.2f\n", tid, duration);
    }
#endif
    /* inform main thread */
    Event_data demul_finish_event;
    demul_finish_event.event_type = EVENT_DEMUL;
    demul_finish_event.data = offset;
    consumer_.handle(demul_finish_event);
}

void DoDemul::DemulSingleSC(int offset)
{
    double start_time = get_time();
    int frame_id, total_data_subframe_id, current_data_subframe_id, sc_id;
    interpreteOffset3d(offset, &frame_id, &current_data_subframe_id, &sc_id);
    int demul_block_size = config_->demul_block_size;
    sc_id *= demul_block_size;
    total_data_subframe_id = current_data_subframe_id + frame_id * config_->data_symbol_num_perframe;
    // interpreteOffset3d(OFDM_DATA_NUM, offset, &frame_id, &total_data_subframe_id, &current_data_subframe_id, &sc_id);
    // int subframe_offset = subframe_num_perframe * frame_id + UE_NUM + current_data_subframe_id;

    int transpose_block_size = config_->transpose_block_size;
    int gather_step_size = 8 * transpose_block_size;

#if DEBUG_PRINT_IN_TASK
    printf("In doDemul thread %d: frame: %d, subframe: %d, subcarrier: %d \n", tid, frame_id, current_data_subframe_id, sc_id);
#endif

    __m256i index = _mm256_setr_epi32(0, 1, transpose_block_size * 2, transpose_block_size * 2 + 1, transpose_block_size * 4, transpose_block_size * 4 + 1, transpose_block_size * 6, transpose_block_size * 6 + 1);

    int cur_block_id = sc_id / transpose_block_size;
    int sc_inblock_idx = sc_id % transpose_block_size;
    int cur_sc_offset = cur_block_id * transpose_block_size * BS_ANT_NUM + sc_inblock_idx;
    float* tar_data_ptr = (float*)spm_buffer;
    float* src_data_ptr = (float*)data_buffer_[total_data_subframe_id] + cur_sc_offset * 2;
    for (int ant_idx = 0; ant_idx < BS_ANT_NUM; ant_idx += 4) {
        __m256 data_rx = _mm256_i32gather_ps(src_data_ptr, index, 4);
        _mm256_store_ps(tar_data_ptr, data_rx);
        src_data_ptr += gather_step_size;
        tar_data_ptr += 8;
    }

    // mat_data size: BS_ANT_NUM \times 1
    cx_float* data_ptr = (cx_float*)(spm_buffer);
    cx_fmat mat_data(data_ptr, BS_ANT_NUM, 1, false);
    // cout<< "Raw data: " << mat_data.st()<<endl;

    // mat_precoder size: UE_NUM \times BS_ANT_NUM
    int precoder_offset = frame_id * OFDM_DATA_NUM + sc_id;
    cx_float* precoder_ptr = (cx_float*)precoder_buffer_[precoder_offset];

    cx_fmat mat_precoder(precoder_ptr, UE_NUM, BS_ANT_NUM, false);
    // cout<<"Precoder: "<< mat_precoder<<endl;

    // mat_demuled size: UE_NUM \times 1
    cx_float* equal_ptr = (cx_float*)(&equal_buffer_[total_data_subframe_id][sc_id * UE_NUM]);
    cx_fmat mat_equaled(equal_ptr, UE_NUM, 1, false);

    // Demodulation
    // sword* demul_ptr = (sword *)(&demod_hard_buffer_.data[total_data_subframe_id][sc_id * UE_NUM]);
    // imat mat_demuled(demul_ptr, UE_NUM, 1, false);
    uint8_t* demul_ptr = (&demod_hard_buffer_[total_data_subframe_id][sc_id * UE_NUM]);

    // Equalization
    mat_equaled = mat_precoder * mat_data;
    // cout << "Equaled data: "<<mat_equaled.st()<<endl;

    // Hard decision
    demod_16qam_hard_loop((float*)equal_ptr, demul_ptr, UE_NUM);
    printf("In doDemul thread %d: frame: %d, subframe: %d, subcarrier: %d \n", tid, frame_id, current_data_subframe_id, sc_id);
    cout << "Demuled data: ";
    for (int ue_idx = 0; ue_idx < UE_NUM; ue_idx++) {
        cout << *(demul_ptr + ue_idx) << "  ";
    }
    cout << endl;

    // inform main thread
    double duration3 = get_time() - start_time;
    Demul_task_duration[tid][1] += duration3;
    Event_data demul_finish_event;
    demul_finish_event.event_type = EVENT_DEMUL;
    demul_finish_event.data = offset;
    Demul_task_count[tid] = Demul_task_count[tid] + 1;
    consumer_.handle(demul_finish_event);
    double duration = get_time() - start_time;
    Demul_task_duration[tid][0] += duration;
}
