#include "pch.h"
#include "FFmpegTranscode.h"
#include "FFmpegTranscode.g.cpp"
#include "FFmpegTranscodeInputCropRectangle.g.cpp"
#include "FFmpegTranscodeInputCropEntry.g.cpp"
#include "FFmpegTranscodeInput.g.cpp"
#include "FFmpegTranscodeOutput.g.cpp"

namespace winrt::FFmpegInteropX::implementation
{
    using namespace std;

    template <typename T, auto ReleaseFunc>
    struct AutoReleasePtr
    {
        T* ptr = nullptr;

        AutoReleasePtr() = default;
        AutoReleasePtr(T* p) : ptr(p) { }

        T** operator&() { return &ptr; }
        T* operator->() { return ptr; }
        T& operator*() { return *ptr; }
        operator bool() const { return ptr; }

        ~AutoReleasePtr()
        {
            if (ptr)
            {
                if constexpr (is_same_v<decltype(ReleaseFunc), void(*)(T*)>)
                    ReleaseFunc(ptr);
                else
                    ReleaseFunc(&ptr);
            }
        }
    };

    static AVCodecID GetCodecId(OutputType type)
    {
        switch (type)
        {
        case OutputType::Mp4:
            return AV_CODEC_ID_H264;
        case OutputType::Vp8:
            return AV_CODEC_ID_VP8;
        case OutputType::Vp9:
            return AV_CODEC_ID_VP9;
        default:
            throw_hresult(E_INVALIDARG);
        }
    }

    struct FilterContext
    {
        AVFilterContext* buffersink_ctx = {};
        AVFilterContext* buffersrc_ctx = {};
        AVFilterGraph* filter_graph = {};
    };

    static int encode_write_frame(AVFrame& filteredFrame,
        AVFormatContext& outputFormatContext, AVCodecContext& outputCodecContext, AVPacket& outputPacket, bool flush)
    {
        av_packet_unref(&outputPacket);
        auto ret = avcodec_send_frame(&outputCodecContext, flush ? nullptr : &filteredFrame);
        while (ret >= 0)
        {
            ret = avcodec_receive_packet(&outputCodecContext, &outputPacket);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                ret = 0;
                break;
            }

            outputPacket.stream_index = 0;
            av_packet_rescale_ts(&outputPacket, outputCodecContext.time_base, outputFormatContext.streams[0]->time_base);
            ret = av_interleaved_write_frame(&outputFormatContext, &outputPacket);
        }

        return ret;
    }

    void FFmpegTranscode::Run(FFmpegInteropX::FFmpegTranscodeInput input, FFmpegInteropX::FFmpegTranscodeOutput output)
    {
        // open input
        AutoReleasePtr<AVFormatContext, avformat_close_input> inputFormatContext;
        if (avformat_open_input(&inputFormatContext, StringUtils::PlatformStringToUtf8String(input.FileName()).c_str(),
            nullptr, nullptr) < 0)
        {
            throw_hresult(E_FAIL);
        }

        if (avformat_find_stream_info(&*inputFormatContext, nullptr) < 0)
            throw_hresult(E_FAIL);

        auto inputCodecPar = inputFormatContext->streams[input.VideoStreamIndex()]->codecpar;
        auto inputCodec = avcodec_find_decoder(inputCodecPar->codec_id);
        if (!inputCodec)
            throw_hresult(E_FAIL);

        AutoReleasePtr<AVCodecContext, avcodec_free_context> inputCodecContext = avcodec_alloc_context3(inputCodec);
        if (avcodec_parameters_to_context(&*inputCodecContext, inputCodecPar) < 0)
            throw_hresult(E_FAIL);

        if (avcodec_open2(&*inputCodecContext, inputCodec, nullptr) < 0)
            throw_hresult(E_FAIL);

        av_dump_format(&*inputFormatContext, 0, StringUtils::PlatformStringToUtf8String(input.FileName()).c_str(), 0);

        // open output
        AutoReleasePtr<AVFormatContext, avformat_free_context> outputFormatContext;
        if (avformat_alloc_output_context2(&outputFormatContext, nullptr, nullptr,
            StringUtils::PlatformStringToUtf8String(output.FileName()).c_str()) < 0)
        {
            throw_hresult(E_FAIL);
        }

        // build output codec
        auto outputCodec = avcodec_find_encoder(GetCodecId(output.Type()));
        if (!outputCodec)
            throw_hresult(E_FAIL);

        AutoReleasePtr<AVCodecContext, avcodec_free_context> outputCodecContext = avcodec_alloc_context3(outputCodec);
        if (!outputCodecContext)
            throw_hresult(E_FAIL);

        outputCodecContext->bit_rate = output.Bitrate();
        outputCodecContext->width = (int)output.PixelSize().Width;
        outputCodecContext->height = (int)output.PixelSize().Height;
        outputCodecContext->time_base = { 10000, (int)(output.FrameRate() / 10000) };
        outputCodecContext->gop_size = 10;  // I-frame interval
        outputCodecContext->max_b_frames = 1;
        outputCodecContext->pix_fmt = inputCodecContext->pix_fmt;

        //H264:  av_opt_set(c->priv_data, "preset", "slow", 0);

        if (avcodec_open2(&*outputCodecContext, outputCodec, nullptr) < 0)
            throw_hresult(E_FAIL);

        if (outputFormatContext->oformat->flags & AVFMT_GLOBALHEADER)
            outputCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        av_dump_format(&*outputFormatContext, 0, StringUtils::PlatformStringToUtf8String(output.FileName()).c_str(), 1);

        // open the output file
        if (!(outputFormatContext->oformat->flags & AVFMT_NOFILE))
        {
            if (avio_open(&outputFormatContext->pb, StringUtils::PlatformStringToUtf8String(output.FileName()).c_str(), AVIO_FLAG_WRITE) < 0)
                throw_hresult(E_FAIL);
        }

        // write the stream header
        if (avformat_write_header(&*outputFormatContext, nullptr) < 0)
            throw_hresult(E_FAIL);

        //auto filterContexts = make_unique<FilterContext[]>(inputFormatContext->nb_streams);

        // build filter graph
        AutoReleasePtr<AVFilterInOut, avfilter_inout_free> filterInputs = avfilter_inout_alloc();
        AutoReleasePtr<AVFilterInOut, avfilter_inout_free> filterOutputs = avfilter_inout_alloc();
        AutoReleasePtr<AVFilterGraph, avfilter_graph_free> filterGraph = avfilter_graph_alloc();
        if (!filterInputs || !filterOutputs || !filterGraph)
            throw_hresult(E_FAIL);

        auto bufferSource = avfilter_get_by_name("buffer");
        auto bufferSink = avfilter_get_by_name("buffersink");
        if (!bufferSource || !bufferSink)
            throw_hresult(E_FAIL);

        auto args = std::format("video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}",
            outputCodecContext->width, outputCodecContext->height, (int)inputCodecContext->pix_fmt,
            outputCodecContext->time_base.num, outputCodecContext->time_base.den,
            inputCodecContext->sample_aspect_ratio.num, inputCodecContext->sample_aspect_ratio.den);

        AVFilterContext* buffersrc_ctx = nullptr, * buffersink_ctx = nullptr;
        if (avfilter_graph_create_filter(&buffersrc_ctx, bufferSource, "in",
            args.c_str(), nullptr, &*filterGraph) < 0)
        {
            throw_hresult(E_FAIL);
        }
        if (avfilter_graph_create_filter(&buffersink_ctx, bufferSink, "out",
            nullptr, nullptr, &*filterGraph) < 0)
        {
            throw_hresult(E_FAIL);
        }
        if (av_opt_set_bin(buffersink_ctx, "pix_fmts",
            (uint8_t*)&outputCodecContext->pix_fmt, sizeof(outputCodecContext->pix_fmt), AV_OPT_SEARCH_CHILDREN) < 0)
        {
            throw_hresult(E_FAIL);
        }

        // endpoints for the filter graph
        filterInputs->name = av_strdup("out");
        filterInputs->filter_ctx = buffersrc_ctx;
        filterInputs->pad_idx = 0;
        filterInputs->next = nullptr;

        filterOutputs->name = av_strdup("in");
        filterOutputs->filter_ctx = buffersink_ctx;
        filterOutputs->pad_idx = 0;
        filterOutputs->next = nullptr;

        if (avfilter_graph_parse_ptr(&*filterGraph, "null", &filterInputs, &filterOutputs, nullptr) < 0)
            throw_hresult(E_FAIL);
        if (avfilter_graph_config(&*filterGraph, nullptr) < 0)
            throw_hresult(E_FAIL);

        AutoReleasePtr<AVPacket, av_packet_unref> inputPacket = av_packet_alloc();
        AutoReleasePtr<AVFrame, av_frame_free> inputFrame = av_frame_alloc();
        AutoReleasePtr<AVPacket, av_packet_unref> outputPacket = av_packet_alloc();
        AutoReleasePtr<AVFrame, av_frame_free> filteredFrame = av_frame_alloc();

        // transcode
        while (true)
        {
            if (av_read_frame(&*inputFormatContext, &*inputPacket) < 0)
                break;

            if (inputPacket->stream_index == input.VideoStreamIndex())
            {
                av_packet_rescale_ts(&*inputPacket, inputFormatContext->streams[input.VideoStreamIndex()]->time_base, inputCodecContext->time_base);

                int ret;
                if ((ret = avcodec_send_packet(&*inputCodecContext, &*inputPacket)) < 0)
                    throw_hresult(E_FAIL);

                while (ret >= 0)
                {
                    ret = avcodec_receive_frame(&*inputCodecContext, &*inputFrame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    else if (ret < 0)
                        throw_hresult(E_FAIL);

                    inputFrame->pts = inputFrame->best_effort_timestamp;

                    // push the decoded frame into the filter graph
                    if (av_buffersrc_add_frame_flags(buffersrc_ctx, &*inputFrame, 0) < 0)
                        throw_hresult(E_FAIL);

                    // pull filtered frames from the filter graph
                    while (1)
                    {
                        ret = av_buffersink_get_frame(buffersink_ctx, &*filteredFrame);
                        if (ret < 0)
                        {
                            // if no more frames, rewrite the code to 0 to show it as normal completion
                            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                                ret = 0;
                            break;
                        }

                        // write the filtered frame to the output file
                        filteredFrame->pict_type = AV_PICTURE_TYPE_NONE;

                        ret = encode_write_frame(*filteredFrame, *outputFormatContext, *outputCodecContext, *outputPacket, false);

                        av_frame_unref(&*filteredFrame);
                        if (ret < 0)
                            break;
                    }

                    if (ret < 0)
                        throw_hresult(E_FAIL);
                }

            }

            av_packet_unref(&*inputPacket);
        }

        // flush stuff
        if (av_buffersrc_add_frame_flags(buffersrc_ctx, nullptr, 0) >= 0)
        {
            while (1)
            {
                auto ret = av_buffersink_get_frame(buffersink_ctx, &*filteredFrame);
                if (ret < 0)
                    break;

                filteredFrame->pict_type = AV_PICTURE_TYPE_NONE;
                if (encode_write_frame(*filteredFrame, *outputFormatContext, *outputCodecContext, *outputPacket, true) < 0)
                    break;
            }
        }

        av_write_trailer(&*outputFormatContext);
    }

    FFmpegTranscode::~FFmpegTranscode()
    {
        Close();
    }

    void FFmpegTranscode::Close()
    {
    }
}