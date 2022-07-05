#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <zimg++.hpp>
#include "VSHelper4.h"
#include "vsxx4_pluginmain.h"

using namespace vsxx4;

namespace {

zimg_pixel_type_e translate_type(const VSVideoFormat &vsformat)
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


class VSUnresize : public FilterBase {
	FilterNode m_clip;
	zimgxx::FilterGraph m_graph;
	VSVideoInfo m_vi;
	size_t m_tmp_size;
public:
	VSUnresize(void * = nullptr) : m_graph{ nullptr }, m_vi{}, m_tmp_size {} {}

	const char *get_name(void *) noexcept override { return "Unresize"; }

	void init(const ConstMap &in, const Map &out, const Core &core) override
	{
		FilterNode clip = in.get_prop<FilterNode>("clip");
		int width = in.get_prop<int>("width");
		int height = in.get_prop<int>("height");
		std::string chromaloc = in.get_prop<std::string>("chromaloc", map::Ignore{});
		double src_left = in.get_prop<double>("src_left", map::Ignore{});
		double src_top = in.get_prop<double>("src_top", map::Ignore{});

		const ::VSVideoInfo &vi = clip.video_info();
		if (!vsh::isConstantVideoFormat(&vi))
			throw std::runtime_error{ "clip must be constant format" };

		zimgxx::zimage_format src_format;
		src_format.width = vi.width;
		src_format.height = vi.height;
		src_format.pixel_type = translate_type(vi.format);
		src_format.subsample_w = vi.format.subSamplingW;
		src_format.subsample_h = vi.format.subSamplingH;
		src_format.color_family = vi.format.colorFamily == cfGray ? ZIMG_COLOR_GREY : ZIMG_COLOR_YUV;
		src_format.depth = vi.format.bitsPerSample;
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
		params.cpu_type = ZIMG_CPU_AUTO_64B;
		params.resample_filter = static_cast<zimg_resample_filter_e>(-1);

		m_clip = std::move(clip);

		m_vi = vi;
		m_vi.width = dst_format.width;
		m_vi.height = dst_format.height;

		try {
			m_graph = zimgxx::FilterGraph{ zimgxx::FilterGraph::build(src_format, dst_format, &params) };
			m_tmp_size = m_graph.get_tmp_size();
		} catch (const zimgxx::zerror &e) {
			throw std::runtime_error{ e.msg };
		}

		create_video_filter(out, m_vi, fmParallel, simple_dep(m_clip, rpStrictSpatial), core);
	}

	ConstFrame get_frame_initial(int n, const Core &core, const FrameContext &frame_context, void *) override
	{
		frame_context.request_frame(n, m_clip);
		return nullptr;
	}

	ConstFrame get_frame(int n, const Core &core, const FrameContext &frame_context, void *) override
	{
		ConstFrame src = frame_context.get_frame(n, m_clip);
		std::unique_ptr<void, decltype(&vsh::vsh_aligned_free)> tmp(vsh::vsh_aligned_malloc(m_tmp_size, 64), vsh::vsh_aligned_free);

		if (!tmp)
			throw std::runtime_error{ "error allocating temporary buffer" };

		Frame dst = core.new_video_frame(m_vi.format, m_vi.width, m_vi.height, src);

		zimgxx::zimage_buffer_const src_buf;
		zimgxx::zimage_buffer dst_buf;

		for (int p = 0; p < m_vi.format.numPlanes; ++p) {
			src_buf.data(p) = src.read_ptr(p);
			src_buf.stride(p) = src.stride(p);
			src_buf.mask(p) = ZIMG_BUFFER_MAX;

			dst_buf.data(p) = dst.write_ptr(p);
			dst_buf.stride(p) = dst.stride(p);
			dst_buf.mask(p) = ZIMG_BUFFER_MAX;
		}

		try {
			m_graph.process(src_buf, dst_buf, tmp.get());
		} catch (const zimgxx::zerror &e) {
			throw std::runtime_error{ e.msg };
		}

		return dst;
	}
};

const PluginInfo4 g_plugin_info4{
	"vsunresize", "unresize", "ghostbusters_2016", 0,
	{
		{ &FilterBase::filter_create<VSUnresize>, "Unresize",
			"clip:vnode;width:int;height:int;chromaloc:data:opt;src_left:float:opt;src_top:float:opt;", "clip:vnode;" }
	}
};
