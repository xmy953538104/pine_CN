        if (diagnostics::ResourceObservationEnabled()) {
            diagnostics::ResourceAdd(diagnostics::ResourceMetric::HostVmaAllocationBytes, allocationInfo.size);
            diagnostics::ResourceAdd(diagnostics::ResourceMetric::HostVmaAllocations);
        }
