            try {
                std::scoped_lock lock{gpu.queueMutex};
                gpu.vkQueue.submit(submitInfo, cycle->fence);
                if (diagnostics::ResourceObservationEnabled())
                    diagnostics::ResourceAdd(diagnostics::ResourceMetric::HostSubmitAccepted);
            } catch (const vk::DeviceLostError &) {
                if (diagnostics::ResourceObservationEnabled())
                    diagnostics::ResourceAdd(diagnostics::ResourceMetric::HostSubmitErrors);
                std::this_thread::sleep_for(std::chrono::seconds(5));
                throw exception("Vulkan device lost!");
            } catch (...) {
                if (diagnostics::ResourceObservationEnabled())
                    diagnostics::ResourceAdd(diagnostics::ResourceMetric::HostSubmitErrors);
                throw;
            }
