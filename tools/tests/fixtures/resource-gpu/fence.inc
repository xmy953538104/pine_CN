                if (!diagnostics::ResourceObservationEnabled()) {
                    cycle->Wait(true);
                    return;
                }
                const auto started{std::chrono::steady_clock::now()};
                diagnostics::ResourceAdd(diagnostics::ResourceMetric::HostFenceWaitEntered);
                const auto recordElapsed = [&] {
                    diagnostics::ResourceAdd(diagnostics::ResourceMetric::HostFenceWaits);
                    diagnostics::ResourceAdd(diagnostics::ResourceMetric::HostFenceWaitUs,
                        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
                };
                try {
                    cycle->Wait(true);
                } catch (const signal::SignalException &e) {
                    recordElapsed();
                    if (e.signal != SIGINT)
                        diagnostics::ResourceAdd(diagnostics::ResourceMetric::HostFenceErrors);
                    throw;
                } catch (...) {
                    recordElapsed();
                    diagnostics::ResourceAdd(diagnostics::ResourceMetric::HostFenceErrors);
                    throw;
                }
                recordElapsed();
