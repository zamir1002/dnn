#pragma once
#include "Layer.h"

namespace dnn
{
	class BatchNormRelu final : public Layer
	{
	private:
		dnnl::normalization_flags Flags;
		std::unique_ptr<dnnl::batch_normalization_forward::primitive_desc> fwdDesc;
		std::unique_ptr<dnnl::batch_normalization_backward::primitive_desc> bwdDesc;
		std::unique_ptr<dnnl::binary::primitive_desc> bwdAddDesc;
		std::unique_ptr<dnnl::memory> WorkspaceMemory;
		std::unique_ptr<dnnl::batch_normalization_forward> fwd;
		std::unique_ptr<dnnl::batch_normalization_backward> bwd;
		std::unique_ptr<dnnl::binary> bwdAdd;

		FloatVector ScaleShift;
		FloatVector DiffScaleShift;

		bool inference;
		bool reorderFwdSrc;
		bool reorderBwdSrc;
		bool reorderBwdDiffSrc;

	public:
		const bool Scaling;
		const Float Eps;
		const Float Momentum;
		const Float OneMinusMomentum;

		FloatVector Mean;
		FloatVector RunningMean;
		FloatVector Variance;
		FloatVector RunningVariance;
		FloatVector InvStdDev;

		BatchNormRelu(const dnn::Device& device, const dnnl::memory::format_tag format, const std::string& name, const std::vector<Layer*>& inputs, const bool scaling = true, const Float momentum = Float(0.99), const Float eps = Float(1e-04), const bool hasBias = true) :
			Layer(device, format, name, LayerTypes::BatchNormRelu, inputs[0]->C, inputs[0]->C, inputs[0]->C, inputs[0]->D, inputs[0]->H, inputs[0]->W, 0, 0, 0, inputs, hasBias),
			Scaling(scaling),
			Eps(eps),
			Momentum(momentum),
			OneMinusMomentum(Float(1) - momentum),
			Flags(dnnl::normalization_flags::fuse_norm_relu),
			inference(false),
			reorderFwdSrc(false),
			reorderBwdSrc(false),
			reorderBwdDiffSrc(false)
		{
			assert(Inputs.size() == 1);

			Mean = FloatVector(PaddedC, Float(0));
			RunningMean = FloatVector(PaddedC, Float(0));
			Variance = FloatVector(PaddedC, Float(1));
			RunningVariance = FloatVector(PaddedC, Float(1));
			InvStdDev = FloatVector(PaddedC);

			if (Scaling)
			{
				ScaleShift = FloatVector(2 * PaddedC, Float(1));
				for (size_t c = 0; c < C; c++)
					ScaleShift[PaddedC + c] = Float(0);

				DiffScaleShift = FloatVector(2 * PaddedC, Float(0));
			}

			WeightsMemDesc = std::make_unique<dnnl::memory::desc>(dnnl::memory::desc(dnnl::memory::dims({ 2, dnnl::memory::dim(C) }), dnnl::memory::data_type::f32, dnnl::memory::format_tag::nc));
			PersistWeightsMemDesc = std::make_unique<dnnl::memory::desc>(dnnl::memory::desc(dnnl::memory::dims({ 2, dnnl::memory::dim(C) }), dnnl::memory::data_type::f32, dnnl::memory::format_tag::nc));
		}

		std::string GetDescription() const final override
		{
			auto description = GetDescriptionHeader() + GetWeightsDescription(Scaling);

			description.append(nwl + std::string(" Momentum:") + tab + FloatToString(Momentum));
			description.append(nwl + std::string(" Eps:") + dtab + FloatToStringScientific(Eps));

			auto mean = Float(0);
			auto variance = Float(0);
			for (auto c = 0ull; c < C; c++)
			{
				mean += RunningMean[c];
				variance += RunningVariance[c];
			}
			mean /= C;
			variance /= C;

			description.append(nwl + std::string(" Mean:") + dtab + FloatToStringFixed(mean));
			description.append(nwl + std::string(" Variance:") + tab + FloatToStringFixed(variance));

			return description;
		}

		size_t FanIn() const final override
		{
			return 1;
		}

		size_t FanOut() const final override
		{
			return 1;
		}

		void InitializeDescriptors(const size_t batchSize) final override
		{
			if (InputLayer->DstMemDesc->data.ndims == 2)
			{
				if (Format == dnnl::memory::format_tag::any)
					chosenFormat = dnnl::memory::format_tag::nc;

				DstMemDesc = std::make_unique<dnnl::memory::desc>(dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(batchSize), dnnl::memory::dim(C) }), dnnl::memory::data_type::f32, chosenFormat));
				DiffDstMemDesc = std::make_unique<dnnl::memory::desc>(dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(batchSize), dnnl::memory::dim(C) }), dnnl::memory::data_type::f32, chosenFormat));
			}
			else
			{
				if (Format == dnnl::memory::format_tag::any)
				{
					chosenFormat = GetDataFmt(*InputLayer->DstMemDesc);
					if (chosenFormat != GetDataFmt(*InputLayer->DiffDstMemDesc))
						throw std::invalid_argument(std::string("Src and Diff format are different in ") + std::string(magic_enum::enum_name<LayerTypes>(LayerType)) + std::string(" layer ") + Name);
				}

				DstMemDesc = std::make_unique<dnnl::memory::desc>(dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(batchSize), dnnl::memory::dim(C), dnnl::memory::dim(H), dnnl::memory::dim(W) }), dnnl::memory::data_type::f32, chosenFormat));
				DiffDstMemDesc = std::make_unique<dnnl::memory::desc>(dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(batchSize), dnnl::memory::dim(C), dnnl::memory::dim(H), dnnl::memory::dim(W) }), dnnl::memory::data_type::f32, chosenFormat));
			}

			if (inference)
				Flags = Scaling ? dnnl::normalization_flags::fuse_norm_relu | dnnl::normalization_flags::use_global_stats | dnnl::normalization_flags::use_scale_shift : dnnl::normalization_flags::fuse_norm_relu | dnnl::normalization_flags::use_global_stats;
			else
				Flags = Scaling ? dnnl::normalization_flags::fuse_norm_relu | dnnl::normalization_flags::use_scale_shift : dnnl::normalization_flags::fuse_norm_relu;

			fwdDesc = std::make_unique<dnnl::batch_normalization_forward::primitive_desc>(dnnl::batch_normalization_forward::primitive_desc(dnnl::batch_normalization_forward::desc(inference ? dnnl::prop_kind::forward_inference : dnnl::prop_kind::forward, *DstMemDesc, Eps, Flags), Device.engine));

			reorderFwdSrc = fwdDesc->src_desc() != *InputLayer->DstMemDesc;

			fwd = std::make_unique<dnnl::batch_normalization_forward>(dnnl::batch_normalization_forward(*fwdDesc));

			if (!inference)
			{
				WorkspaceMemory = std::make_unique<dnnl::memory>(dnnl::memory(fwdDesc->workspace_desc(), Device.engine));

				bwdDesc = std::make_unique<dnnl::batch_normalization_backward::primitive_desc>(dnnl::batch_normalization_backward::primitive_desc(dnnl::batch_normalization_backward::desc(Scaling ? dnnl::prop_kind::backward : dnnl::prop_kind::backward_data, *DiffDstMemDesc, *DstMemDesc, Eps, Flags), Device.engine, *fwdDesc));

				reorderBwdSrc = bwdDesc->src_desc() != *InputLayer->DstMemDesc;
				reorderBwdDiffSrc = bwdDesc->diff_src_desc() != *InputLayer->DiffDstMemDesc;

				bwd = std::make_unique<dnnl::batch_normalization_backward>(dnnl::batch_normalization_backward(*bwdDesc));
			}

			bwdAddDesc = std::make_unique<dnnl::binary::primitive_desc>(dnnl::binary::primitive_desc(dnnl::binary::desc(dnnl::algorithm::binary_add, *InputLayer->DiffDstMemDesc, *InputLayer->DiffDstMemDesc, *InputLayer->DiffDstMemDesc), Device.engine));
			bwdAdd = std::make_unique<dnnl::binary>(dnnl::binary(*bwdAddDesc));
		}

		bool Lockable() const final override
		{
			return WeightCount > 0 && Scaling;
		}

		void ForwardProp(const size_t batchSize, const bool training) final override
		{
			if (!training)
			{
				if (!inference)
				{
					inference = true;
					InitializeDescriptors(batchSize);
				}

				auto memSrc = dnnl::memory(*InputLayer->DstMemDesc, Device.engine, InputLayer->Neurons.data());
				const auto &srcMem = reorderFwdSrc ? dnnl::memory(fwdDesc->src_desc(), Device.engine) : memSrc;
				if (reorderFwdSrc)
				{
					dnnl::reorder(memSrc, srcMem).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, memSrc}, { DNNL_ARG_TO, srcMem } });
					Device.stream.wait();
				}

				auto memMean = dnnl::memory(fwdDesc->mean_desc(), Device.engine, RunningMean.data());
				auto memVariance = dnnl::memory(fwdDesc->variance_desc(), Device.engine, RunningVariance.data());

				auto dstMem = dnnl::memory(*DstMemDesc, Device.engine, Neurons.data());

				if (Scaling)
				{
					for (auto c = 0ull; c < C; c++)
					{
						ScaleShift[c] = Weights[c];
						ScaleShift[PaddedC + c] = Biases[c];
					}
					auto memScaleShift = dnnl::memory(*WeightsMemDesc, Device.engine, ScaleShift.data());

#ifdef DNN_CACHE_PRIMITIVES
					fwd->execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem}, { DNNL_ARG_MEAN, memMean }, { DNNL_ARG_VARIANCE, memVariance }, { DNNL_ARG_SCALE_SHIFT, memScaleShift }, { DNNL_ARG_DST, dstMem } });
#else
					dnnl::batch_normalization_forward(*fwdDesc).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem}, { DNNL_ARG_MEAN, memMean }, { DNNL_ARG_VARIANCE, memVariance }, { DNNL_ARG_SCALE_SHIFT, memScaleShift }, { DNNL_ARG_DST, dstMem } });
#endif
				}
				else
#ifdef DNN_CACHE_PRIMITIVES
					fwd->execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem}, { DNNL_ARG_MEAN, memMean }, { DNNL_ARG_VARIANCE, memVariance }, { DNNL_ARG_DST, dstMem } });
#else
					dnnl::batch_normalization_forward(*fwdDesc).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem}, { DNNL_ARG_MEAN, memMean }, { DNNL_ARG_VARIANCE, memVariance }, { DNNL_ARG_DST, dstMem } });
#endif
				Device.stream.wait();
			}
			else
			{
				if (inference)
				{
					inference = false;
					InitializeDescriptors(batchSize);
				}

				auto memSrc = dnnl::memory(*InputLayer->DstMemDesc, Device.engine, InputLayer->Neurons.data());
				const auto &srcMem = reorderFwdSrc ? dnnl::memory(fwdDesc->src_desc(), Device.engine) : memSrc;
				if (reorderFwdSrc)
				{
					dnnl::reorder(memSrc, srcMem).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, memSrc}, { DNNL_ARG_TO, srcMem } });
					Device.stream.wait();
				}

				auto memMean = dnnl::memory(fwdDesc->mean_desc(), Device.engine, Mean.data());
				auto memVariance = dnnl::memory(fwdDesc->variance_desc(), Device.engine, Variance.data());

				auto dstMem = dnnl::memory(*DstMemDesc, Device.engine, Neurons.data());

				if (Scaling)
				{
					for (auto c = 0ull; c < C; c++)
					{
						ScaleShift[c] = Weights[c];
						ScaleShift[PaddedC + c] = Biases[c];
					}
					auto memScaleShift = dnnl::memory(*WeightsMemDesc, Device.engine, ScaleShift.data());
#ifdef DNN_CACHE_PRIMITIVES
					fwd->execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem}, { DNNL_ARG_MEAN, memMean }, { DNNL_ARG_VARIANCE, memVariance }, { DNNL_ARG_SCALE_SHIFT, memScaleShift }, { DNNL_ARG_DST, dstMem }, { DNNL_ARG_WORKSPACE, *WorkspaceMemory } });
#else
					dnnl::batch_normalization_forward(*fwdDesc).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem}, { DNNL_ARG_MEAN, memMean }, { DNNL_ARG_VARIANCE, memVariance }, { DNNL_ARG_SCALE_SHIFT, memScaleShift }, { DNNL_ARG_DST, dstMem }, { DNNL_ARG_WORKSPACE, *WorkspaceMemory } });
#endif
				}
				else
#ifdef DNN_CACHE_PRIMITIVES
					fwd->execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem}, { DNNL_ARG_MEAN, memMean }, { DNNL_ARG_VARIANCE, memVariance }, { DNNL_ARG_DST, dstMem }, { DNNL_ARG_WORKSPACE, *WorkspaceMemory } });
#else
					dnnl::batch_normalization_forward(*fwdDesc).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem}, { DNNL_ARG_MEAN, memMean }, { DNNL_ARG_VARIANCE, memVariance }, { DNNL_ARG_DST, dstMem }, { DNNL_ARG_WORKSPACE, *WorkspaceMemory } });
#endif
				Device.stream.wait();

				const auto unbiasedFactor = Float(batchSize * HW) / Float(batchSize * HW - 1);
				for (auto c = 0ull; c < C; c++)
				{
					RunningMean[c] = (Momentum * RunningMean[c]) + (OneMinusMomentum * Mean[c]);
					RunningVariance[c] = (Momentum * RunningVariance[c]) + (OneMinusMomentum * Variance[c] * unbiasedFactor);
				}

#ifndef DNN_LEAN
				ZeroFloatVector(NeuronsD1.data(), batchSize * PaddedCDHW);
#else
				DNN_UNREF_PAR(batchSize);
#endif // DNN_LEAN
			}
		}

		void BackwardProp(const size_t batchSize) final override
		{
#ifdef DNN_LEAN
			ZeroGradient(batchSize);
#else
			DNN_UNREF_PAR(batchSize);
#endif // DNN_LEAN

			auto memSrc = dnnl::memory(*InputLayer->DstMemDesc, Device.engine, InputLayer->Neurons.data());
			const auto &srcMem = reorderBwdSrc ? dnnl::memory(bwdDesc->src_desc(), Device.engine) : memSrc;
			if (reorderBwdSrc)
			{
				dnnl::reorder(memSrc, srcMem).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, memSrc}, { DNNL_ARG_TO, srcMem } });
				Device.stream.wait();
			}

			auto diffDstMem = dnnl::memory(*DiffDstMemDesc, Device.engine, NeuronsD1.data());

			auto memMean = dnnl::memory(bwdDesc->mean_desc(), Device.engine, Mean.data());
			auto memVariance = dnnl::memory(bwdDesc->variance_desc(), Device.engine, Variance.data());

			const auto &memDiffSrc = SharesInput ? dnnl::memory(*InputLayer->DiffDstMemDesc, Device.engine) : dnnl::memory(*InputLayer->DiffDstMemDesc, Device.engine, InputLayer->NeuronsD1.data());
			const auto &diffSrcMem = reorderBwdDiffSrc ? dnnl::memory(bwdDesc->diff_src_desc(), Device.engine) : memDiffSrc;

			if (Scaling)
			{
				for (auto c = 0ull; c < 2 * PaddedC; c++)
					DiffScaleShift[c] = Float(0);

				auto scaleShiftMemory = dnnl::memory(*WeightsMemDesc, Device.engine, ScaleShift.data());
				auto diffScaleShiftMemory = dnnl::memory(*WeightsMemDesc, Device.engine, DiffScaleShift.data());

				bwd->execute(Device.stream, std::unordered_map<int, dnnl::memory> { {DNNL_ARG_SRC, srcMem}, { DNNL_ARG_DIFF_DST, diffDstMem }, { DNNL_ARG_MEAN, memMean }, { DNNL_ARG_VARIANCE, memVariance }, { DNNL_ARG_SCALE_SHIFT, scaleShiftMemory }, { DNNL_ARG_WORKSPACE, *WorkspaceMemory }, { DNNL_ARG_DIFF_SRC, diffSrcMem }, { DNNL_ARG_DIFF_SCALE_SHIFT, diffScaleShiftMemory } });

				for (auto c = 0ull; c < C; c++)
				{
					WeightsD1[c] += DiffScaleShift[c];
					BiasesD1[c] += DiffScaleShift[PaddedC + c];
				}
			}
			else
				bwd->execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem}, { DNNL_ARG_DIFF_DST, diffDstMem }, { DNNL_ARG_MEAN, memMean }, { DNNL_ARG_VARIANCE, memVariance }, { DNNL_ARG_WORKSPACE, *WorkspaceMemory }, { DNNL_ARG_DIFF_SRC, diffSrcMem } });

			Device.stream.wait();

			if (reorderBwdDiffSrc)
			{
				dnnl::reorder(diffSrcMem, memDiffSrc).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, diffSrcMem}, { DNNL_ARG_TO, memDiffSrc } });
				Device.stream.wait();
			}

			if (SharesInput)
			{
				bwdAdd->execute(Device.stream, std::unordered_map<int, dnnl::memory>{ { DNNL_ARG_SRC_0, dnnl::memory(*InputLayer->DiffDstMemDesc, Device.engine, InputLayer->NeuronsD1.data()) }, { DNNL_ARG_SRC_1, memDiffSrc }, { DNNL_ARG_DST, dnnl::memory(*InputLayer->DiffDstMemDesc, Device.engine, InputLayer->NeuronsD1.data()) } });
				Device.stream.wait();
			}

#ifdef DNN_LEAN
			ReleaseGradient();
#endif // DNN_LEAN		
		}

		ByteVector GetImage(const Byte fillColor) final override
		{
			if (Scaling)
			{
				const auto rangeWeights = GetColorRange(WeightsMin, WeightsMax);
				const auto rangeBiases = GetColorRange(BiasesMin, BiasesMax);

				const auto width = BiasCount;
				const auto height = WeightCount / BiasCount;
				const auto totalSize = width * (height + 3);

				auto image = ByteVector(totalSize, fillColor);

				for (auto y = 0ull; y < height; y++)
				{
					const auto start = y * width;
					const auto end = start + width;
					for (auto x = start; x < end; x++)
						image[x] = GetColorFromRange(rangeWeights, WeightsMin, Weights[x]);
				}

				if (HasBias)
				{
					const auto offset = (height + 1) * width;
					for (auto x = 0ull; x < width; x++)
						image[x + offset] = GetColorFromRange(rangeBiases, BiasesMin, Biases[x]);
				}

				return image;
			}
			else
				return ByteVector();
		}

		void ResetWeights(const Fillers weightFiller, const Float weightFillerScale, const Fillers biasFiller, const Float biasFillerScale) override
		{
			Weights = FloatVector(PaddedC, Float(1));
			Biases = FloatVector(PaddedC, Float(0));

			RunningMean = FloatVector(PaddedC, Float(0));
			RunningVariance = FloatVector(PaddedC, Float(1));
		}

		void Save(std::ostream& os, const bool persistOptimizer = false, const Optimizers optimizer = Optimizers::SGD) override
		{
			os.write(reinterpret_cast<const char*>(RunningMean.data()), std::streamsize(C * sizeof(Float)));
			os.write(reinterpret_cast<const char*>(RunningVariance.data()), std::streamsize(C * sizeof(Float)));

			if (Scaling)
			{
				for (size_t c = 0; c < C; c++)
				{
					Weights[c] = ScaleShift[c];
					Biases[c] = ScaleShift[C + c];
				}
			}
			
			Layer::Save(os, persistOptimizer, optimizer);
		}

		void Load(std::istream& is, const bool persistOptimizer = false, const Optimizers optimizer = Optimizers::SGD) override
		{
			is.read(reinterpret_cast<char*>(RunningMean.data()), std::streamsize(C * sizeof(Float)));
			is.read(reinterpret_cast<char*>(RunningVariance.data()), std::streamsize(C * sizeof(Float)));

			Layer::Load(is, persistOptimizer, optimizer);

			if (Scaling)
			{
				for (size_t c = 0; c < C; c++)
				{
					ScaleShift[c] = Weights[c];
					ScaleShift[C + c] = Biases[c];
				}
			}
		}

		std::streamsize GetWeightsSize(const bool persistOptimizer = false, const Optimizers optimizer = Optimizers::SGD) const override
		{
			return (2 * C * sizeof(Float)) + Layer::GetWeightsSize(persistOptimizer, optimizer);
		}
	};
}
