#include <stdio.h>
#include <string.h>
#include <time.h>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <inttypes.h>
#include <cinttypes>
#include <algorithm>
#include <filesystem>

#include "model_adapter.h"

#include "flux.hpp"
#include "stable-diffusion.cpp"
#include "util.cpp"
#include "upscaler.cpp"
#include "model.cpp"
#include "zip.c"

#include "otherarch/utils.h"

// #include "preprocessing.hpp"
#include "stable-diffusion.h"

//#define STB_IMAGE_IMPLEMENTATION //already defined in llava
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

// #define STB_IMAGE_RESIZE_IMPLEMENTATION //already defined in llava
#include "stb_image_resize.h"

enum SDMode {
    TXT2IMG,
    IMG2IMG,
    IMG2VID,
    CONVERT,
    MODE_COUNT
};

struct SDParams {
    int n_threads = -1;
    SDMode mode   = TXT2IMG;
    std::string model_path;
    std::string clip_l_path;
    std::string clip_g_path;
    std::string t5xxl_path;
    std::string diffusion_model_path;
    std::string vae_path;
    std::string taesd_path;
    std::string esrgan_path;
    std::string controlnet_path;
    std::string embeddings_path;
    std::string stacked_id_embeddings_path;
    std::string input_id_images_path = "";
    sd_type_t wtype = SD_TYPE_COUNT;
    std::string lora_model_dir;
    std::string output_path = "output.png";
    std::string input_path;
    std::string mask_path;
    std::string control_image_path;

    std::string prompt;
    std::string negative_prompt;
    float min_cfg     = 1.0f;
    float cfg_scale   = 7.0f;
    float guidance    = 3.5f;
    float eta         = 0.f;
    float style_ratio = 20.f;
    int clip_skip     = -1;  // <= 0 represents unspecified
    int width         = 512;
    int height        = 512;
    int batch_count   = 1;

    int video_frames         = 6;
    int motion_bucket_id     = 127;
    int fps                  = 6;
    float augmentation_level = 0.f;

    sample_method_t sample_method = EULER_A;
    schedule_t schedule           = DEFAULT;
    int sample_steps              = 20;
    float strength                = 0.75f;
    float control_strength        = 0.9f;
    rng_type_t rng_type           = CUDA_RNG;
    int64_t seed                  = 42;
    bool verbose                  = false;
    bool vae_tiling               = false;
    bool control_net_cpu          = false;
    bool normalize_input          = false;
    bool clip_on_cpu              = false;
    bool vae_on_cpu               = false;
    bool diffusion_flash_attn     = false;
    bool canny_preprocess         = false;
    bool color                    = false;
    int upscale_repeats           = 1;

    std::vector<int> skip_layers = {7, 8, 9};
    float slg_scale              = 0.f;
    float skip_layer_start       = 0.01f;
    float skip_layer_end         = 0.2f;
};

//shared
int total_img_gens = 0;

//global static vars for SD
static SDParams * sd_params = nullptr;
static sd_ctx_t * sd_ctx = nullptr;
static int sddebugmode = 0;
static std::string recent_data = "";
static uint8_t * input_image_buffer = NULL;
static uint8_t * input_mask_buffer = NULL;
static uint8_t * input_photomaker_buffer = NULL;

static std::string sdplatformenv, sddeviceenv, sdvulkandeviceenv;
static int cfg_tiled_vae_threshold = 0;
static int cfg_square_limit = 0;
static int cfg_side_limit = 0;
static bool sd_is_quiet = false;
static std::string sdmodelfilename = "";
static bool photomaker_enabled = false;

bool sdtype_load_model(const sd_load_model_inputs inputs) {
    sd_is_quiet = inputs.quiet;
    set_sd_quiet(sd_is_quiet);
    executable_path = inputs.executable_path;
    std::string taesdpath = "";
    std::string lorafilename = inputs.lora_filename;
    std::string vaefilename = inputs.vae_filename;
    std::string t5xxl_filename = inputs.t5xxl_filename;
    std::string clipl_filename = inputs.clipl_filename;
    std::string clipg_filename = inputs.clipg_filename;
    std::string photomaker_filename = inputs.photomaker_filename;
    cfg_tiled_vae_threshold = inputs.tiled_vae_threshold;
    cfg_tiled_vae_threshold = (cfg_tiled_vae_threshold > 8192 ? 8192 : cfg_tiled_vae_threshold);
    cfg_tiled_vae_threshold = (cfg_tiled_vae_threshold <= 0 ? 8192 : cfg_tiled_vae_threshold); //if negative dont tile
    cfg_side_limit = inputs.img_hard_limit;
    cfg_square_limit = inputs.img_soft_limit;
    printf("\nImageGen Init - Load Model: %s\n",inputs.model_filename);

    if(lorafilename!="")
    {
        printf("With LoRA: %s at %f power\n",lorafilename.c_str(),inputs.lora_multiplier);
    }
    if(inputs.taesd)
    {
        taesdpath = executable_path + "taesd.embd";
        printf("With TAE SD VAE: %s\n",taesdpath.c_str());
    }
    else if(vaefilename!="")
    {
        printf("With Custom VAE: %s\n",vaefilename.c_str());
    }
    if(t5xxl_filename!="")
    {
        printf("With Custom T5-XXL Model: %s\n",t5xxl_filename.c_str());
    }
    if(clipl_filename!="")
    {
        printf("With Custom Clip-L Model: %s\n",clipl_filename.c_str());
    }
    if(clipg_filename!="")
    {
        printf("With Custom Clip-G Model: %s\n",clipg_filename.c_str());
    }
    if(photomaker_filename!="")
    {
        printf("With PhotoMaker Model: %s\n",photomaker_filename.c_str());
        photomaker_enabled = true;
    }
    if(inputs.quant)
    {
        printf("Note: Loading a pre-quantized model is always faster than using compress weights!\n");
    }

    //duplicated from expose.cpp
    int cl_parseinfo = inputs.clblast_info; //first digit is whether configured, second is platform, third is devices
    std::string usingclblast = "GGML_OPENCL_CONFIGURED="+std::to_string(cl_parseinfo>0?1:0);
    putenv((char*)usingclblast.c_str());
    cl_parseinfo = cl_parseinfo%100; //keep last 2 digits
    int platform = cl_parseinfo/10;
    int devices = cl_parseinfo%10;
    sdplatformenv = "GGML_OPENCL_PLATFORM="+std::to_string(platform);
    sddeviceenv = "GGML_OPENCL_DEVICE="+std::to_string(devices);
    putenv((char*)sdplatformenv.c_str());
    putenv((char*)sddeviceenv.c_str());
    std::string vulkan_info_raw = inputs.vulkan_info;
    std::string vulkan_info_str = "";
    for (size_t i = 0; i < vulkan_info_raw.length(); ++i) {
        vulkan_info_str += vulkan_info_raw[i];
        if (i < vulkan_info_raw.length() - 1) {
            vulkan_info_str += ",";
        }
    }
    if(vulkan_info_str!="")
    {
        sdvulkandeviceenv = "GGML_VK_VISIBLE_DEVICES="+vulkan_info_str;
        putenv((char*)sdvulkandeviceenv.c_str());
    }

    sd_params = new SDParams();
    sd_params->model_path = inputs.model_filename;
    sd_params->wtype = (inputs.quant==0?SD_TYPE_COUNT:SD_TYPE_Q4_0);
    sd_params->n_threads = inputs.threads; //if -1 use physical cores
    sd_params->input_path = ""; //unused
    sd_params->batch_count = 1;
    sd_params->vae_path = vaefilename;
    sd_params->taesd_path = taesdpath;
    sd_params->t5xxl_path = t5xxl_filename;
    sd_params->clip_l_path = clipl_filename;
    sd_params->clip_g_path = clipg_filename;
    sd_params->stacked_id_embeddings_path = photomaker_filename;
    //if t5 is set, and model is a gguf, load it as a diffusion model path
    bool endswithgguf = (sd_params->model_path.rfind(".gguf") == sd_params->model_path.size() - 5);
    if(sd_params->t5xxl_path!="" && endswithgguf)
    {
        printf("\nSwap to Diffusion Model Path:%s",sd_params->model_path.c_str());
        sd_params->diffusion_model_path = sd_params->model_path;
        sd_params->model_path = "";
    }

    sddebugmode = inputs.debugmode;

    set_sd_log_level(sddebugmode);

    bool vae_decode_only = false;
    bool free_param = false;
    if(inputs.debugmode==1)
    {
        printf("\nMODEL:%s\nVAE:%s\nTAESD:%s\nCNET:%s\nLORA:%s\nEMBD:%s\nVAE_DEC:%d\nVAE_TILE:%d\nFREE_PARAM:%d\nTHREADS:%d\nWTYPE:%d\nRNGTYPE:%d\nSCHED:%d\nCNETCPU:%d\n\n",
        sd_params->model_path.c_str(),
        sd_params->vae_path.c_str(),
        sd_params->taesd_path.c_str(),
        sd_params->controlnet_path.c_str(),
        sd_params->lora_model_dir.c_str(),
        sd_params->embeddings_path.c_str(),
        vae_decode_only,
        sd_params->vae_tiling,
        free_param,
        sd_params->n_threads,
        sd_params->wtype,
        sd_params->rng_type,
        sd_params->schedule,
        sd_params->control_net_cpu);
    }

    sd_ctx = new_sd_ctx(sd_params->model_path.c_str(),
                        sd_params->clip_l_path.c_str(),
                        sd_params->clip_g_path.c_str(),
                        sd_params->t5xxl_path.c_str(),
                        sd_params->diffusion_model_path.c_str(),
                        sd_params->vae_path.c_str(),
                        sd_params->taesd_path.c_str(),
                        sd_params->controlnet_path.c_str(),
                        sd_params->lora_model_dir.c_str(),
                        sd_params->embeddings_path.c_str(),
                        sd_params->stacked_id_embeddings_path.c_str(),
                        vae_decode_only,
                        sd_params->vae_tiling,
                        free_param,
                        sd_params->n_threads,
                        sd_params->wtype,
                        sd_params->rng_type,
                        sd_params->schedule,
                        sd_params->clip_on_cpu,
                        sd_params->control_net_cpu,
                        sd_params->vae_on_cpu,
                        sd_params->diffusion_flash_attn);

    if (sd_ctx == NULL) {
        printf("\nError: KCPP SD Failed to create context!\nIf using Flux/SD3.5, make sure you have ALL files required (e.g. VAE, T5, Clip...) or baked in!\n");
        return false;
    }

    std::filesystem::path mpath(inputs.model_filename);
    sdmodelfilename = mpath.filename().string();

    if(lorafilename!="" && inputs.lora_multiplier>0)
    {
        printf("\nApply LoRA...\n");
       // sd_ctx->sd->set_pending_lora(lorafilename,inputs.lora_multiplier);
        sd_ctx->sd->apply_lora_from_file(lorafilename,inputs.lora_multiplier);
    }

    return true;

}

std::string clean_input_prompt(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (char ch : input) {
        // Check if the character is an ASCII or extended ASCII character
        if (static_cast<unsigned char>(ch) <= 0x7F || (ch >= 0xC2 && ch <= 0xF4)) {
            result.push_back(ch);
        }
    }
    //limit to max 800 chars
    result = result.substr(0, 800);
    return result;
}

static std::string get_image_params(const SDParams& params) {
    std::string parameter_string = "";
    parameter_string += "Prompt: " + params.prompt + " | ";
    parameter_string += "NegativePrompt: " + params.negative_prompt + " | ";
    parameter_string += "Steps: " + std::to_string(params.sample_steps) + " | ";
    parameter_string += "CFGScale: " + std::to_string(params.cfg_scale) + " | ";
    parameter_string += "Guidance: " + std::to_string(params.guidance) + " | ";
    parameter_string += "Seed: " + std::to_string(params.seed) + " | ";
    parameter_string += "Size: " + std::to_string(params.width) + "x" + std::to_string(params.height) + " | ";
    parameter_string += "Sampler: " + std::to_string((int)sd_params->sample_method) + " | ";
    parameter_string += "Clip skip: " + std::to_string((int)sd_params->clip_skip) + " | ";
    parameter_string += "Model: " + sdmodelfilename + " | ";
    parameter_string += "Version: KoboldCpp";
    return parameter_string;
}

static inline int rounddown_64(int n) {
    return n - n % 64;
}

static inline int roundup_64(int n) {
    return ((n + 63) / 64) * 64;
}

//scale dimensions to ensure width and height stay within limits
//img_hard_limit = sdclamped, hard size limit per side, no side can exceed this
//square limit = total NxN resolution based limit to also apply
static void sd_fix_resolution(int &width, int &height, int img_hard_limit, int img_soft_limit) {

    // sanitize the original values
    width = std::max(std::min(width, 8192), 64);
    height = std::max(std::min(height, 8192), 64);

    bool is_landscape = (width > height);
    int long_side = is_landscape ? width : height;
    int short_side = is_landscape ? height : width;
    float original_ratio = static_cast<float>(long_side) / short_side;

    // for the initial rounding, don't bother comparing to the original
    // requested ratio, since the user can choose those values directly
    long_side = rounddown_64(long_side);
    short_side = rounddown_64(short_side);
    img_hard_limit = rounddown_64(img_hard_limit);

    //enforce sdclamp side limit
    if (long_side > img_hard_limit) {
        short_side = static_cast<int>(short_side * img_hard_limit / static_cast<float>(long_side));
        long_side = img_hard_limit;
        if (short_side <= 64) {
            short_side = 64;
        } else {
            int down = rounddown_64(short_side);
            int up = roundup_64(short_side);
            float longf = static_cast<float>(long_side);
            // Choose better ratio match between rounding up or down
            short_side = (longf / down - original_ratio < original_ratio - longf / up) ? down : up;
        }
    }

    //enforce sd_restrict_square area limit
    int area_limit = img_soft_limit * img_soft_limit;
    if (long_side * short_side > area_limit) {
        float scale = std::sqrt(static_cast<float>(area_limit) / (long_side * short_side));
        int new_short = static_cast<int>(short_side * scale);
        int new_long = static_cast<int>(long_side * scale);

        if (new_short <= 64) {
            short_side = 64;
            long_side = rounddown_64(area_limit / short_side);
        } else {
            int new_long_down = rounddown_64(new_long);
            int new_short_down = rounddown_64(new_short);
            int new_short_up = roundup_64(new_short);
            int new_long_up = roundup_64(new_long);
            long_side = new_long_down;
            short_side = new_short_down;

            // we may get a ratio closer to the original if we still stay below the
            // limit when rounding up one of the dimensions, so check both cases
            float rdiff = std::fabs(static_cast<float>(new_long_down) / new_short_down - original_ratio);

            if (new_long_down * new_short_up < area_limit) {
                float newrdiff = std::fabs(static_cast<float>(new_long_down) / new_short_up - original_ratio);
                if (newrdiff < rdiff) {
                    long_side = new_long_down;
                    short_side = new_short_up;
                    rdiff = newrdiff;
                }
            }

            if (new_long_up * new_short_down < area_limit) {
                float newrdiff = std::fabs(static_cast<float>(new_long_up) / new_short_down - original_ratio);
                if (newrdiff < rdiff) {
                    long_side = new_long_up;
                    short_side = new_short_down;
                }
            }
        }
    }

    if (is_landscape) {
        width = long_side;
        height = short_side;
    } else {
        width = short_side;
        height = long_side;
    }
}

sd_generation_outputs sdtype_generate(const sd_generation_inputs inputs)
{
    sd_generation_outputs output;

    if(sd_ctx == nullptr || sd_params == nullptr)
    {
        printf("\nWarning: KCPP image generation not initialized!\n");
        output.data = "";
        output.status = 0;
        return output;
    }
    sd_image_t * results;
    sd_image_t* control_image = NULL;

    //sanitize prompts, remove quotes and limit lengths
    std::string cleanprompt = clean_input_prompt(inputs.prompt);
    std::string cleannegprompt = clean_input_prompt(inputs.negative_prompt);
    std::string img2img_data = std::string(inputs.init_images);
    std::string img2img_mask = std::string(inputs.mask);
    std::string photomaker_image_data = std::string(inputs.photomaker_image);
    std::string sampler = inputs.sample_method;

    if(!photomaker_enabled)
    {
        photomaker_image_data = "";
    }

    sd_params->prompt = cleanprompt;
    sd_params->negative_prompt = cleannegprompt;
    sd_params->cfg_scale = inputs.cfg_scale;
    sd_params->sample_steps = inputs.sample_steps;
    sd_params->seed = inputs.seed;
    sd_params->width = inputs.width;
    sd_params->height = inputs.height;
    sd_params->strength = inputs.denoising_strength;
    sd_params->clip_skip = inputs.clip_skip;
    sd_params->mode = (img2img_data==""?SDMode::TXT2IMG:SDMode::IMG2IMG);

    auto loadedsdver = get_loaded_sd_version(sd_ctx);
    if (loadedsdver == SDVersion::VERSION_FLUX)
    {
        if (!sd_loaded_chroma()) {
            sd_params->cfg_scale = 1;  //non chroma clamp cfg scale
        }
        if (sampler == "euler a" || sampler == "k_euler_a" || sampler == "euler_a") {
            sampler = "euler";  //euler a broken on flux
        }
    }

    const int default_res_limit = 8192; // arbitrary, just to simplify the code
    // avoid crashes due to bugs/limitations on certain models
    // although it can be possible for a single side to exceed 1024, the total resolution of the image
    // cannot exceed (832x832) for sd1/sd2 or (1024x1024) for sdxl/sd3/flux, to prevent crashing the server
    const int hard_megapixel_res_limit = (loadedsdver==SDVersion::VERSION_SD1 || loadedsdver==SDVersion::VERSION_SD2)?832:1024;

    int img_hard_limit = default_res_limit;
    if (cfg_side_limit > 0) {
        img_hard_limit = std::max(std::min(cfg_side_limit, default_res_limit), 64);
    }

    int img_soft_limit = default_res_limit;
    if (cfg_square_limit > 0) {
        img_soft_limit = std::max(std::min(cfg_square_limit, default_res_limit), 64);
    }

    if (cfg_square_limit > 0 && sddebugmode == 1) {
        img_soft_limit = std::min(hard_megapixel_res_limit * 2, img_soft_limit);  //double the limit for debugmode if cfg_square_limit is set
    } else {
        img_soft_limit = std::min(hard_megapixel_res_limit, img_soft_limit);
    }

    sd_fix_resolution(sd_params->width, sd_params->height, img_hard_limit, img_soft_limit);
    if (inputs.width != sd_params->width || inputs.height != sd_params->height) {
        printf("\nKCPP SD: Requested dimensions %dx%d changed to %dx%d\n", inputs.width, inputs.height, sd_params->width, sd_params->height);
    }

    // trigger tiling by image area, the memory used for the VAE buffer is 6656 bytes per image pixel, default 768x768
    bool dotile = (sd_params->width*sd_params->height > cfg_tiled_vae_threshold*cfg_tiled_vae_threshold);
    set_sd_vae_tiling(sd_ctx,dotile); //changes vae tiling, prevents memory related crash/oom

    if (sd_params->clip_skip <= 0) {
        // workaround for clip_skip being "stuck" at the previous requested value
        // 2 is the default for all recent base models (SD2, SDXL, Flux, SD3)
        if (sd_version_is_sd1((SDVersion)loadedsdver)) {
            sd_params->clip_skip = 1;
        }
        else {
            sd_params->clip_skip = 2;
        }
    }

    //for img2img
    sd_image_t input_image = {0,0,0,nullptr};
    sd_image_t photomaker_reference = {0,0,0,nullptr};
    std::vector<uint8_t> image_buffer;
    std::vector<uint8_t> image_mask_buffer;
    std::vector<uint8_t> photomaker_buffer;
    int nx, ny, nc;
    int img2imgW = sd_params->width; //for img2img input
    int img2imgH = sd_params->height;
    int img2imgC = 3; // Assuming RGB image
    std::vector<uint8_t> resized_image_buf(img2imgW * img2imgH * img2imgC);
    std::vector<uint8_t> resized_mask_buf(img2imgW * img2imgH * img2imgC);
    std::vector<uint8_t> resized_photomaker_buf(img2imgW * img2imgH * img2imgC);

    std::string ts = get_timestamp_str();
    if(!sd_is_quiet)
    {
        printf("\n[%s] Generating Image (%d steps)\n",ts.c_str(),inputs.sample_steps);
    }else{
        printf("\n[%s] Generating (%d st.)\n",ts.c_str(),inputs.sample_steps);
    }

    fflush(stdout);

    if(sampler=="euler a"||sampler=="k_euler_a"||sampler=="euler_a") //all lowercase
    {
        sd_params->sample_method = sample_method_t::EULER_A;
    }
    else if(sampler=="euler"||sampler=="k_euler")
    {
        sd_params->sample_method = sample_method_t::EULER;
    }
    else if(sampler=="heun"||sampler=="k_heun")
    {
        sd_params->sample_method = sample_method_t::HEUN;
    }
    else if(sampler=="dpm2"||sampler=="k_dpm_2")
    {
        sd_params->sample_method = sample_method_t::DPM2;
    }
    else if(sampler=="lcm"||sampler=="k_lcm")
    {
        sd_params->sample_method = sample_method_t::LCM;
    }
    else if(sampler=="ddim")
    {
        sd_params->sample_method = sample_method_t::DDIM_TRAILING;
    }
    else if(sampler=="dpm++ 2m karras" || sampler=="dpm++ 2m" || sampler=="k_dpmpp_2m")
    {
        sd_params->sample_method = sample_method_t::DPMPP2M;
    }
    else
    {
        sd_params->sample_method = sample_method_t::EULER_A;
    }

    if(photomaker_image_data!="")
    {
        if(input_photomaker_buffer!=nullptr) //just in time free old buffer
        {
            stbi_image_free(input_photomaker_buffer);
            input_photomaker_buffer = nullptr;
        }
        int nx2, ny2, nc2;
        photomaker_buffer = kcpp_base64_decode(photomaker_image_data);
        input_photomaker_buffer = stbi_load_from_memory(photomaker_buffer.data(), photomaker_buffer.size(), &nx2, &ny2, &nc2, 1);
        // Resize the image
        int resok = stbir_resize_uint8(input_photomaker_buffer, nx2, ny2, 0, resized_photomaker_buf.data(), img2imgW, img2imgH, 0, 1);
        if (!resok) {
            printf("\nKCPP SD: resize photomaker image failed!\n");
            output.data = "";
            output.status = 0;
            return output;
        }
        photomaker_reference.width = img2imgW;
        photomaker_reference.height = img2imgH;
        photomaker_reference.channel = img2imgC;
        photomaker_reference.data = resized_photomaker_buf.data();

        //ensure prompt has img keyword, otherwise append it
        if (sd_params->prompt.find("img") == std::string::npos) {
            sd_params->prompt += " img";
        }
    }

    if (sd_params->mode == TXT2IMG) {

        if(!sd_is_quiet && sddebugmode==1)
        {
            printf("\nTXT2IMG PROMPT:%s\nNPROMPT:%s\nCLPSKP:%d\nCFGSCLE:%f\nW:%d\nH:%d\nSM:%d\nSTEP:%d\nSEED:%d\nBATCH:%d\nCIMG:%p\nCSTR:%f\n\n",
            sd_params->prompt.c_str(),
            sd_params->negative_prompt.c_str(),
            sd_params->clip_skip,
            sd_params->cfg_scale,
            sd_params->width,
            sd_params->height,
            sd_params->sample_method,
            sd_params->sample_steps,
            (int)sd_params->seed,
            sd_params->batch_count,
            control_image,
            sd_params->control_strength);
        }


        results = txt2img(sd_ctx,
                          sd_params->prompt.c_str(),
                          sd_params->negative_prompt.c_str(),
                          sd_params->clip_skip,
                          sd_params->cfg_scale,
                          sd_params->guidance,
                          sd_params->eta,
                          sd_params->width,
                          sd_params->height,
                          sd_params->sample_method,
                          sd_params->sample_steps,
                          sd_params->seed,
                          sd_params->batch_count,
                          control_image,
                          sd_params->control_strength,
                          sd_params->style_ratio,
                          sd_params->normalize_input,
                          sd_params->input_id_images_path.c_str(),
                          sd_params->skip_layers.data(),
                          sd_params->skip_layers.size(),
                          sd_params->slg_scale,
                          sd_params->skip_layer_start,
                          sd_params->skip_layer_end,
                          (photomaker_image_data!=""?(&photomaker_reference):nullptr));
    } else {

        if (sd_params->width <= 0 || sd_params->width % 64 != 0 || sd_params->height <= 0 || sd_params->height % 64 != 0) {
            printf("\nKCPP SD: bad request image dimensions!\n");
            output.data = "";
            output.status = 0;
            return output;
        }

        image_buffer = kcpp_base64_decode(img2img_data);
        if(input_image_buffer!=nullptr) //just in time free old buffer
        {
             stbi_image_free(input_image_buffer);
             input_image_buffer = nullptr;
        }
        input_image_buffer = stbi_load_from_memory(image_buffer.data(), image_buffer.size(), &nx, &ny, &nc, 3);

        if (nx < 64 || ny < 64 || nx > 1024 || ny > 1024 || nc!= 3) {
            printf("\nKCPP SD: bad input image dimensions %d x %d!\n",nx,ny);
            output.data = "";
            output.status = 0;
            return output;
        }
        if (!input_image_buffer) {
            printf("\nKCPP SD: load image from memory failed!\n");
            output.data = "";
            output.status = 0;
            return output;
        }

        // Resize the image
        int resok = stbir_resize_uint8(input_image_buffer, nx, ny, 0, resized_image_buf.data(), img2imgW, img2imgH, 0, img2imgC);
        if (!resok) {
            printf("\nKCPP SD: resize image failed!\n");
            output.data = "";
            output.status = 0;
            return output;
        }

        if(img2img_mask!="")
        {
            int nx2, ny2, nc2;
            if(input_mask_buffer!=nullptr) //just in time free old buffer
            {
                stbi_image_free(input_mask_buffer);
                input_mask_buffer = nullptr;
            }
            image_mask_buffer = kcpp_base64_decode(img2img_mask);
            input_mask_buffer = stbi_load_from_memory(image_mask_buffer.data(), image_mask_buffer.size(), &nx2, &ny2, &nc2, 1);
            // Resize the image
            int resok = stbir_resize_uint8(input_mask_buffer, nx2, ny2, 0, resized_mask_buf.data(), img2imgW, img2imgH, 0, 1);
            if (!resok) {
                printf("\nKCPP SD: resize image failed!\n");
                output.data = "";
                output.status = 0;
                return output;
            }
            if(inputs.flip_mask)
            {
                int bufsiz = resized_mask_buf.size();
                for (int i = 0; i < bufsiz; ++i) {
                    resized_mask_buf[i] = 255 - resized_mask_buf[i];
                }
            }
        }

        input_image.width = img2imgW;
        input_image.height = img2imgH;
        input_image.channel = img2imgC;
        input_image.data = resized_image_buf.data();

        uint8_t* mask_image_buffer    = NULL;
        std::vector<uint8_t> default_mask_image_vec(img2imgW * img2imgH * img2imgC, 255);
        if (img2img_mask != "") {
            mask_image_buffer = resized_mask_buf.data();
        } else {
            mask_image_buffer = default_mask_image_vec.data();
        }
        sd_image_t mask_image = { (uint32_t) img2imgW, (uint32_t) img2imgH, 1, mask_image_buffer };

        if(!sd_is_quiet && sddebugmode==1)
        {
            printf("\nIMG2IMG PROMPT:%s\nNPROMPT:%s\nCLPSKP:%d\nCFGSCLE:%f\nW:%d\nH:%d\nSM:%d\nSTEP:%d\nSEED:%d\nBATCH:%d\nCIMG:%p\nSTR:%f\n\n",
            sd_params->prompt.c_str(),
            sd_params->negative_prompt.c_str(),
            sd_params->clip_skip,
            sd_params->cfg_scale,
            sd_params->width,
            sd_params->height,
            sd_params->sample_method,
            sd_params->sample_steps,
            (int)sd_params->seed,
            sd_params->batch_count,
            control_image,
            sd_params->strength);
        }

        results = img2img(sd_ctx,
                            input_image,
                            mask_image,
                            sd_params->prompt.c_str(),
                            sd_params->negative_prompt.c_str(),
                            sd_params->clip_skip,
                            sd_params->cfg_scale,
                            sd_params->guidance,
                            sd_params->eta,
                            sd_params->width,
                            sd_params->height,
                            sd_params->sample_method,
                            sd_params->sample_steps,
                            sd_params->strength,
                            sd_params->seed,
                            sd_params->batch_count,
                            control_image,
                            sd_params->control_strength,
                            sd_params->style_ratio,
                            sd_params->normalize_input,
                            sd_params->input_id_images_path.c_str(),
                            sd_params->skip_layers.data(),
                            sd_params->skip_layers.size(),
                            sd_params->slg_scale,
                            sd_params->skip_layer_start,
                            sd_params->skip_layer_end,
                            (photomaker_image_data!=""?(&photomaker_reference):nullptr));
    }

    if (results == NULL) {
        printf("\nKCPP SD generate failed!\n");
        output.data = "";
        output.status = 0;
        return output;
    }


    for (int i = 0; i < sd_params->batch_count; i++) {
        if (results[i].data == NULL) {
            continue;
        }

        int out_data_len;
        unsigned char * png = stbi_write_png_to_mem(results[i].data, 0, results[i].width, results[i].height, results[i].channel, &out_data_len, get_image_params(*sd_params).c_str());
        if (png != NULL)
        {
            recent_data = kcpp_base64_encode(png,out_data_len);
            free(png);
        }

        free(results[i].data);
        results[i].data = NULL;
    }

    free(results);
    output.data = recent_data.c_str();
    output.status = 1;
    total_img_gens += 1;
    return output;
}
