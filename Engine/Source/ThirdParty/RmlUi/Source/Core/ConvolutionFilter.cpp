#include "../../Include/RmlUi/Core/ConvolutionFilter.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include <float.h>
#include <string.h>
#include <vector>

namespace Rml {

namespace {
	struct KernelTap {
		int x;
		int y;
		float weight;
	};

	template <FilterOperation Operation>
	void RunKernel(byte* destination, const Vector2i destination_dimensions, const int destination_stride, const int destination_bytes_per_pixel,
		const int destination_alpha_offset, const byte* source, const Vector2i source_dimensions, const Vector2i source_offset,
		const int source_bytes_per_pixel, const int source_alpha_offset, const std::vector<KernelTap>& taps, const Vector2i kernel_radius)
	{
		const int source_row_bytes = source_dimensions.x * source_bytes_per_pixel;
		for (int y = 0; y < destination_dimensions.y; ++y)
		{
			const int center_y = y - source_offset.y;
			const bool row_outside = center_y + kernel_radius.y < 0 || center_y - kernel_radius.y >= source_dimensions.y;
			const bool row_inside = center_y - kernel_radius.y >= 0 && center_y + kernel_radius.y < source_dimensions.y;

			for (int x = 0; x < destination_dimensions.x; ++x)
			{
				const int center_x = x - source_offset.x;
				float opacity = 0.f;

				// A window that misses the source entirely reads nothing, which is most of an outline's margin.
				if (!row_outside && center_x + kernel_radius.x >= 0 && center_x - kernel_radius.x < source_dimensions.x)
				{
					const bool inside = row_inside && center_x - kernel_radius.x >= 0 && center_x + kernel_radius.x < source_dimensions.x;
					const int center_index = center_y * source_row_bytes + center_x * source_bytes_per_pixel + source_alpha_offset;
					for (const KernelTap& tap : taps)
					{
						const int source_x = center_x + tap.x;
						const int source_y = center_y + tap.y;
						if (!inside && (source_x < 0 || source_x >= source_dimensions.x || source_y < 0 || source_y >= source_dimensions.y))
							continue;

						const float pixel_opacity = float(source[center_index + tap.y * source_row_bytes + tap.x * source_bytes_per_pixel]) * tap.weight;
						if (Operation == FilterOperation::Sum)
						{
							opacity += pixel_opacity;
						}
						else
						{
							opacity = Math::Max(opacity, pixel_opacity);
							if (opacity >= 255.f)
								break;
						}
					}
				}

				opacity = Math::Min(255.f, opacity);
				destination[y * destination_stride + x * destination_bytes_per_pixel + destination_alpha_offset] = byte(opacity);
			}
		}
	}
} // namespace

ConvolutionFilter::ConvolutionFilter() {}

ConvolutionFilter::~ConvolutionFilter() {}

bool ConvolutionFilter::Initialise(int _kernel_radius, FilterOperation _operation)
{
	return Initialise(Vector2i(_kernel_radius), _operation);
}

bool ConvolutionFilter::Initialise(Vector2i _kernel_radii, FilterOperation _operation)
{
	if (_kernel_radii.x < 0 || _kernel_radii.y < 0)
	{
		RMLUI_ERRORMSG("Invalid input parameters to convolution filter.");
		return false;
	}

	kernel_size = _kernel_radii * 2 + Vector2i(1);

	kernel = UniquePtr<float[]>(new float[kernel_size.x * kernel_size.y]());

	operation = _operation;
	return true;
}

float* ConvolutionFilter::operator[](int kernel_y_index)
{
	RMLUI_ASSERT(kernel != nullptr && kernel_y_index >= 0 && kernel_y_index < kernel_size.y);

	kernel_y_index = Math::Clamp(kernel_y_index, 0, kernel_size.y - 1);

	return kernel.get() + kernel_size.x * kernel_y_index;
}

void ConvolutionFilter::Run(byte* destination, const Vector2i destination_dimensions, const int destination_stride,
	const ColorFormat destination_color_format, const byte* source, const Vector2i source_dimensions, const Vector2i source_offset,
	const ColorFormat source_color_format) const
{
	RMLUI_ZoneScopedNC("ConvFilter::Run", 0xd6bf49);

	const int destination_bytes_per_pixel = (destination_color_format == ColorFormat::RGBA8 ? 4 : 1);
	const int destination_alpha_offset = (destination_color_format == ColorFormat::RGBA8 ? 3 : 0);
	const int source_bytes_per_pixel = (source_color_format == ColorFormat::RGBA8 ? 4 : 1);
	const int source_alpha_offset = (source_color_format == ColorFormat::RGBA8 ? 3 : 0);

	const Vector2i kernel_radius = (kernel_size - Vector2i(1)) / 2;

	// A zero weight adds nothing to a sum and cannot raise a maximum, so only the other taps are visited.
	std::vector<KernelTap> taps;
	taps.reserve(size_t(kernel_size.x * kernel_size.y));
	for (int kernel_y = 0; kernel_y < kernel_size.y; ++kernel_y)
	{
		for (int kernel_x = 0; kernel_x < kernel_size.x; ++kernel_x)
		{
			const float weight = kernel[kernel_y * kernel_size.x + kernel_x];
			if (weight != 0.f)
				taps.push_back(KernelTap{kernel_x - kernel_radius.x, kernel_y - kernel_radius.y, weight});
		}
	}

	if (operation == FilterOperation::Sum)
	{
		RunKernel<FilterOperation::Sum>(destination, destination_dimensions, destination_stride, destination_bytes_per_pixel, destination_alpha_offset,
			source, source_dimensions, source_offset, source_bytes_per_pixel, source_alpha_offset, taps, kernel_radius);
	}
	else
	{
		RunKernel<FilterOperation::Dilation>(destination, destination_dimensions, destination_stride, destination_bytes_per_pixel, destination_alpha_offset,
			source, source_dimensions, source_offset, source_bytes_per_pixel, source_alpha_offset, taps, kernel_radius);
	}
}

} // namespace Rml
