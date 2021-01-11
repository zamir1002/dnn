#pragma once
#include "Layer.h"

namespace dnn
{
	class Average final : public Layer
	{
	private:
		std::vector<Float> Scales;
		std::vector<dnnl::memory::desc> srcsMemsDesc;
		std::unordered_map<int, dnnl::memory> fwdArgs;
		std::unique_ptr<dnnl::sum::primitive_desc> fwdDesc;
#ifdef DNN_CACHE_PRIMITIVES
		std::unique_ptr<dnnl::sum> fwd;
#endif

	public:
		const Float Scale;

		Average(const dnn::Device& device, const dnnl::memory::format_tag format, const std::string& name, const std::vector<Layer*>& inputs) :
			Layer(device, format, name, LayerTypes::Average, 0, 0, inputs[0]->C, inputs[0]->D, inputs[0]->H, inputs[0]->W, 0, 0, 0, inputs),
			Scale(Float(1) / Inputs.size())
		{
			assert(Inputs.size() > 1);

			for (auto i = 0ull; i < Inputs.size(); i++)
			{
				assert(Inputs[i]->C == C);
				assert(Inputs[i]->D == D);
				assert(Inputs[i]->H == H);
				assert(Inputs[i]->W == W);
			}

			Scales = std::vector<Float>(Inputs.size(), Scale);
		}

		std::string GetDescription() const final override
		{
			auto description = GetDescriptionHeader();

			description.append(nwl + std::string(" Scale:") + dtab + FloatToString(Scale));

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
						throw std::invalid_argument("Src and Diff format are different in " + std::string(magic_enum::enum_name<LayerTypes>(LayerType)) + " layer " + Name);
				}

				DstMemDesc = std::make_unique<dnnl::memory::desc>(dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(batchSize), dnnl::memory::dim(C), dnnl::memory::dim(H), dnnl::memory::dim(W) }), dnnl::memory::data_type::f32, chosenFormat));
				DiffDstMemDesc = std::make_unique<dnnl::memory::desc>(dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(batchSize), dnnl::memory::dim(C), dnnl::memory::dim(H), dnnl::memory::dim(W) }), dnnl::memory::data_type::f32, chosenFormat));
			}

			for (auto i = 1ull; i < Inputs.size(); i++)
			{
				assert(*DstMemDesc == *Inputs[i]->DstMemDesc);
				if (*DstMemDesc != *Inputs[i]->DstMemDesc)
					throw std::invalid_argument("Incompatible memory formats in Average layer");
			}

			srcsMemsDesc = std::vector<dnnl::memory::desc>(Inputs.size());
			for (auto i = 0ull; i < Inputs.size(); i++)
				srcsMemsDesc[i] = *Inputs[i]->DstMemDesc;

			fwdDesc = std::make_unique<dnnl::sum::primitive_desc>(dnnl::sum::primitive_desc(*DstMemDesc, Scales, srcsMemsDesc, Device.engine));

			fwdArgs = std::unordered_map<int, dnnl::memory>{ { DNNL_ARG_DST, dnnl::memory(*DstMemDesc, Device.engine, Neurons.data()) } };
			for (auto i = 0ull; i < Inputs.size(); i++)
				fwdArgs.insert({ DNNL_ARG_MULTIPLE_SRC + int(i), dnnl::memory(srcsMemsDesc[i], Device.engine, Inputs[i]->Neurons.data()) });

#ifdef DNN_CACHE_PRIMITIVES
			fwd = std::make_unique<dnnl::sum>(dnnl::sum(*fwdDesc));
#endif
		}

		void ForwardProp(const size_t batchSize, const bool training) final override
		{
#ifdef DNN_CACHE_PRIMITIVES
			fwd->execute(Device.stream, fwdArgs);
#else
			dnnl::sum(*fwdDesc).execute(Device.stream, fwdArgs);
#endif
			Device.stream.wait();

#ifndef DNN_LEAN
			if (training)
				ZeroFloatVector(NeuronsD1.data(), batchSize * PaddedCDHW);
#else
			DNN_UNREF_PAR(batchSize);
#endif
		}

		void BackwardProp(const size_t batchSize) final override
		{
#ifdef DNN_LEAN
			ZeroGradientMulti(batchSize);
#endif // DNN_LEAN

			const auto size = IsPlainFormat() ? CDHW : PaddedCDHW;
			const auto part = (size / VectorSize) * VectorSize;
			const auto inputs = Inputs.size();

#ifdef DNN_STOCHASTIC
			if (batchSize == 1)
			{
				for (auto n = 0ull; n < part; n += VectorSize)
					for (auto i = 0ull; i < inputs; i++)
						mul_add(VecFloat().load_a(&NeuronsD1[n]), Scale, VecFloat().load_a(&Inputs[i]->NeuronsD1[n])).store_a(&Inputs[i]->NeuronsD1[n]);

				for (auto n = part; n < size; n++)
					for (auto i = 0ull; i < inputs; i++)
						Inputs[i]->NeuronsD1[n] += Scale * NeuronsD1[n];
			}
			else
			{
#endif
				switch (inputs)
				{
				case 2:
				{
					for_i(batchSize, LIGHT_COMPUTE, [=](size_t b)
					{
						const auto start = b * size;
						const auto end = start + part;

						for (auto n = start; n < end; n+=VectorSize)
						{
							mul_add(VecFloat().load_a(&NeuronsD1[n]), Scale, VecFloat().load_a(&Inputs[0]->NeuronsD1[n])).store_a(&Inputs[0]->NeuronsD1[n]);
							mul_add(VecFloat().load_a(&NeuronsD1[n]), Scale, VecFloat().load_a(&Inputs[1]->NeuronsD1[n])).store_a(&Inputs[1]->NeuronsD1[n]);
						}
						for (auto n = end; n < start + size; n++)
						{
							Inputs[0]->NeuronsD1[n] += Scale * NeuronsD1[n];
							Inputs[1]->NeuronsD1[n] += Scale * NeuronsD1[n];
						}
					});
				}
				break;

				case 3:
				{
					for_i(batchSize, LIGHT_COMPUTE, [=](size_t b)
					{
						const auto start = b * size;
						const auto end = start + part;

						for (auto n = start; n < end; n += VectorSize)
						{
							mul_add(VecFloat().load_a(&NeuronsD1[n]), Scale, VecFloat().load_a(&Inputs[0]->NeuronsD1[n])).store_a(&Inputs[0]->NeuronsD1[n]);
							mul_add(VecFloat().load_a(&NeuronsD1[n]), Scale, VecFloat().load_a(&Inputs[1]->NeuronsD1[n])).store_a(&Inputs[1]->NeuronsD1[n]);
							mul_add(VecFloat().load_a(&NeuronsD1[n]), Scale, VecFloat().load_a(&Inputs[2]->NeuronsD1[n])).store_a(&Inputs[2]->NeuronsD1[n]);
						}
						for (auto n = end; n < start + size; n++)
						{
							Inputs[0]->NeuronsD1[n] += Scale * NeuronsD1[n];
							Inputs[1]->NeuronsD1[n] += Scale * NeuronsD1[n];
							Inputs[2]->NeuronsD1[n] += Scale * NeuronsD1[n];
						}
					});
				}
				break;

				default:
					for_i(batchSize, LIGHT_COMPUTE, [=](size_t b)
					{
						const auto start = b * size;
						const auto end = start + part;

						for (auto n = start; n < end; n += VectorSize)
							for (auto i = 0ull; i < inputs; i++)
								mul_add(VecFloat().load_a(&NeuronsD1[n]), Scale, VecFloat().load_a(&Inputs[i]->NeuronsD1[n])).store_a(&Inputs[i]->NeuronsD1[n]);
							
						for (auto n = end; n < start + size; n++)
							for (auto i = 0ull; i < inputs; i++)
								Inputs[i]->NeuronsD1[n] += Scale * NeuronsD1[n];
					});
				}
#ifdef DNN_STOCHASTIC
			}
#endif

#ifdef DNN_LEAN
			ReleaseGradient();
#endif // DNN_LEAN
		}
	};
}
