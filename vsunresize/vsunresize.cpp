#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <zimg++.hpp>
#include "vsxx_pluginmain.h"

using namespace vsxx;

namespace {

zimg_pixel_type_e translate_type(const ::VSFormat &vsformat)
{
	if (vsformat.sampleType == stInteger && vsformat.bytesPerSample == 1)
		return ZIMG_PIXEL_BYTE;
	else if (vsformat.sampleType == stInteger && vsformat.bytesPerSample == 2)
		return ZIMG_PIXEL_WORD;
	else if (vsformat.sampleType == stFloat && vsformat.bytesPerSample == 2)
		return ZIMG_PIXEL_HALF;
	else if (vsformat.sampleType == stFloat && vsformat.bytesPerSample == 4)
		return ZIMG_PIXEL_FLOAT;
	else
		throw std::runtime_error{ "unsupported pixel type" };
}

} // namespace


class VSUnresize : public vsxx::FilterBase {
	FilterNode m_clip;
	std::unique_ptr<zimgxx::FilterGraph> m_graph;
	VSVideoInfo m_vi;
	size_t m_tmp_size;
public:
	explicit VSUnresize(void *) : m_tmp_size{} {}

	const char *get_name(int) noexcept override { return "Unresize"; }

	std::pair<::VSFilterMode, int> init(const ConstPropertyMap &in, const PropertyMap &out, const VapourCore &core) override
	{
		FilterNode clip = in.get_prop<FilterNode>("clip");
		int width = in.get_prop<int>("width");
		int height = in.get_prop<int>("height");
		std::string chromaloc = in.get_prop<std::string>("chromaloc", map::Ignore{});
		double src_left = in.get_prop<double>("src_left", map::Ignore{});
		double src_top = in.get_prop<double>("src_top", map::Ignore{});

		const ::VSVideoInfo &vi = clip.video_info();
		if (!isConstantFormat(&vi))
			throw std::runtime_error{ "clip must be constant format" };

		zimgxx::zimage_format src_format;
		src_format.width = vi.width;
		src_format.height = vi.height;
		src_format.pixel_type = translate_type(*vi.format);
		src_format.subsample_w = vi.format->subSamplingW;
		src_format.subsample_h = vi.format->subSamplingH;
		src_format.color_family = vi.format->colorFamily == cmGray ? ZIMG_COLOR_GREY : ZIMG_COLOR_YUV;
		src_format.depth = vi.format->bitsPerSample;
		src_format.pixel_range = ZIMG_RANGE_LIMITED;

		if (chromaloc == "jpeg" || chromaloc == "mpeg1" || chromaloc == "center")
			src_format.chroma_location = ZIMG_CHROMA_CENTER;
		else if (chromaloc == "mpeg2" || chromaloc == "left")
			src_format.chroma_location = ZIMG_CHROMA_LEFT;
		else
			src_format.chroma_location = ZIMG_CHROMA_LEFT;

		src_format.active_region.left = src_left;
		src_format.active_region.top = src_top;

		zimgxx::zimage_format dst_format = src_format;
		dst_format.width = width;
		dst_format.height = height;
		dst_format.active_region = { NAN, NAN, NAN, NAN };

		zimgxx::zfilter_graph_builder_params params;
		params.resample_filter = static_cast<zimg_resample_filter_e>(-1);

		m_clip = std::move(clip);

		m_vi = vi;
		m_vi.width = dst_format.width;
		m_vi.height = dst_format.height;

		try {
			m_graph.reset(new zimgxx::FilterGraph{ zimgxx::FilterGraph::build(src_format, dst_format, &params) });
			m_tmp_size = m_graph->get_tmp_size();
		} catch (const zimgxx::zerror &e) {
			throw std::runtime_error{ e.msg };
		}

		return{ fmParallel, 1 };
	}

	std::pair<const ::VSVideoInfo *, size_t> get_video_info() noexcept override
	{
		return{ &m_vi, 1 };
	}

	ConstVideoFrame get_frame_initial(int n, const VapourCore &, ::VSFrameContext *frame_ctx) override
	{
		m_clip.request_frame_filter(n, frame_ctx);
		return ConstVideoFrame{};
	}

	ConstVideoFrame get_frame(int n, const VapourCore &core, ::VSFrameContext *frame_ctx) override
	{
		ConstVideoFrame src = m_clip.get_frame_filter(n, frame_ctx);
		std::unique_ptr<void, void(*)(void *)> tmp(vs_aligned_malloc(m_tmp_size, 32), vs_aligned_free);

		if (!tmp)
			throw std::runtime_error{ "error allocating temporary buffer" };

		VideoFrame dst = core.new_video_frame(*m_vi.format, m_vi.width, m_vi.height, src);

		zimgxx::zimage_buffer_const src_buf;
		zimgxx::zimage_buffer dst_buf;

		for (int p = 0; p < m_vi.format->numPlanes; ++p) {
			src_buf.data(p) = src.read_ptr(p);
			src_buf.stride(p) = src.stride(p);
			src_buf.mask(p) = ZIMG_BUFFER_MAX;

			dst_buf.data(p) = dst.write_ptr(p);
			dst_buf.stride(p) = dst.stride(p);
			dst_buf.mask(p) = ZIMG_BUFFER_MAX;
		}

		try {
			m_graph->process(src_buf, dst_buf, tmp.get());
		} catch (const zimgxx::zerror &e) {
			throw std::runtime_error{ e.msg };
		}

		return dst;
	}
};

const PluginInfo g_plugin_info{
	"vsunresize", "unresize", "ghostbusters_2016",
	{
		{ vsxx::FilterBase::filter_create<VSUnresize>, "Unresize", "clip:clip;width:int;height:int;chromaloc:data:opt;src_left:float:opt;src_top:float:opt;" }
	}
};
