SDRAM Throughput Testing: Test Types, Measurement and Validation
Testing SDRAM throughput involves more than measuring how quickly data can be written to or read from memory. A proper validation process should evaluate:
1. Read bandwidth – How quickly SDRAM can supply data to the processor.
2. Write bandwidth – How quickly the processor can write data into SDRAM.
3. Read/Write bandwidth – Performance when read and write operations happen together.
4. Sequential access performance – Performance when accessing consecutive memory locations.
5. Random access performance – Performance when accessing non-contiguous memory locations.
6. Burst transfer performance – Efficiency of SDRAM burst transactions.
7. Sustained bandwidth – Whether SDRAM maintains its expected performance over extended periods.
8. Concurrent access performance – Performance when multiple bus masters access SDRAM simultaneously.
The objective is to validate both functional correctness and actual memory bandwidth against the theoretical bandwidth of the SDRAM interface.
1. Understand the theoretical SDRAM bandwidth
Before conducting measurements, calculate the theoretical maximum bandwidth.

The theoretical bandwidth is:
\[
BW = \frac{Data\ Bus\ Width}{8} \times Frequency \times Transfers\ Per\ Clock
\]
Where:
- Data Bus Width = SDRAM data bus width in bits.
- Frequency = SDRAM clock frequency.
- Transfers Per Clock = Number of data transfers per clock cycle.
For example, consider a 32-bit SDRAM interface operating at 200 MHz with one transfer per clock:
\[
BW = \frac{32}{8} \times 200 \times 10^6
\]
\[
BW = 800\ MB/s
\]
For a DDR interface operating at the same 200 MHz clock:
\[
BW = 4 \times 200 \times 10^6
\]
\[
BW = 1600\ MB/s
\]
Here, DDR performs two data transfers per clock cycle.

Important: Distinguish between SDRAM clock frequency and data rate. DDR's advertised transfer rate is generally expressed in MT/s, not MHz.

The theoretical bandwidth is an upper bound. Actual bandwidth will be lower because of:
- SDRAM refresh cycles.
- Row activation and precharge delays.
- Read/write turnaround delays.
- Memory controller arbitration.
- Bus contention.
- CPU cache behavior.
- DMA transfer overhead.
- Software overhead.
2. Types of SDRAM tests
I recommend dividing SDRAM testing into five categories.
2.1 Functional and data-integrity tests
These tests verify that SDRAM stores and retrieves data correctly.

They are essential before performing throughput measurements.
Test	Description	What it validates
Walking 1s	Write a single bit set to 1 and move it across all bit positions	Data line connectivity
Walking 0s	Write a single bit set to 0 and move it across all bit positions	Data line connectivity
Checkerboard	Write alternating patterns such as 0xAAAAAAAA and 0x55555555	Data line shorts and coupling
Address test	Write address-dependent values to different locations	Address line correctness
Random pattern	Write pseudo-random data and verify it	Data retention and corruption
March test	Perform sequences of write, read and write operations	Memory cell faults
Data retention	Write patterns, wait, and read them back	Data retention issues
Stress test	Continuously write and read large memory regions	Intermittent memory failures


Recommended implementation:
- Test the entire SDRAM region, or divide it into manageable blocks.
- Use multiple data patterns.
- Run multiple iterations.
- Compare every read value against the expected value.
- Record the failing address, expected value and actual value.
For U-Boot-based platforms, mtest is a useful starting point. It supports memory writes, reads and comparison using configurable address ranges and patterns. docs.u-boot.org
2.2 Sequential read throughput test
Objective: Measure the maximum achievable SDRAM read bandwidth.

Implementation:
1. Allocate a large memory buffer in SDRAM.
2. Initialize it with known data.
3. Sequentially read the entire buffer.
4. Repeat the operation several times.
5. Measure the elapsed time.
Example:
#define BUFFER_SIZE (16 * 1024 * 1024)

volatile uint32_t *buffer = SDRAM_BASE;
uint64_t checksum = 0;

uint64_t start = get_timestamp_us();

for (uint32_t i = 0; i < BUFFER_SIZE / 4; i++)
{
    checksum += buffer[i];
}

uint64_t end = get_timestamp_us();

uint64_t elapsed_us = end - start;

double throughput =
    (double)BUFFER_SIZE / elapsed_us;

throughput *= 1e6;

printf("Read Throughput: %.2f MB/s\n",
       throughput / (1024.0 * 1024.0));

The checksum prevents the compiler from eliminating the read operations.

Important considerations:
- Make the buffer significantly larger than the CPU cache.
- Ensure the buffer is actually located in external SDRAM.
- Account for cache effects.
- Use a high-resolution timer.
- Run multiple iterations and calculate average throughput.
- Ensure the final benchmark result is consumed so that compiler optimization cannot eliminate the work.
For a bare-metal implementation, use cache maintenance operations or a suitable non-cacheable memory region when measuring physical SDRAM traffic.
2.3 Sequential write throughput test
Objective: Measure the maximum achievable SDRAM write bandwidth.

Implementation:
1. Allocate a large SDRAM buffer.
2. Sequentially write data into the buffer.
3. Ensure all writes have reached the relevant memory visibility point.
4. Measure the elapsed time.
Example:
#define BUFFER_SIZE (16 * 1024 * 1024)

volatile uint32_t *buffer = SDRAM_BASE;

uint64_t start = get_timestamp_us();

for (uint32_t i = 0; i < BUFFER_SIZE / 4; i++)
{
    buffer[i] = i;
}

memory_barrier();

/* If using a write-back cache, clean the
 * buffer to the required point of coherency.
 */

uint64_t end = get_timestamp_us();

uint64_t elapsed_us = end - start;

double throughput =
    ((double)BUFFER_SIZE / elapsed_us) * 1e6;

printf("Write Throughput: %.2f MB/s\n",
       throughput / (1024.0 * 1024.0));

Important: A memory barrier alone does not necessarily flush dirty cache lines to SDRAM.

If the CPU uses a write-back cache, stores may initially be completed in the cache rather than external SDRAM.

Therefore, distinguish between:
- CPU store throughput.
- Cache write-back throughput.
- Actual SDRAM write throughput.
For physical SDRAM measurements, clean the cache lines and ensure completion before stopping the timer, or use an appropriately configured non-cacheable memory region.
2.4 Simultaneous read/write throughput test
Objective: Measure SDRAM performance when read and write transactions happen together.

This is important because real embedded applications rarely perform only reads or only writes.

Example:
#define BUFFER_SIZE (16 * 1024 * 1024)

volatile uint32_t *src = SDRAM_BASE;
volatile uint32_t *dst =
    (volatile uint32_t *)(SDRAM_BASE + BUFFER_SIZE);

uint64_t start = get_timestamp_us();

for (uint32_t i = 0; i < BUFFER_SIZE / 4; i++)
{
    dst[i] = src[i];
}

memory_barrier();

uint64_t end = get_timestamp_us();

double elapsed = (double)(end - start);

double total_bytes = 2.0 * BUFFER_SIZE;

double throughput =
    (total_bytes / elapsed) * 1e6;

printf("Copy Bandwidth: %.2f MB/s\n",
       throughput / (1024.0 * 1024.0));

Here, both read and write traffic are counted.

For example:
- 16 MB read.
- 16 MB written.
- 32 MB total transferred.
If the operation takes 40 ms:
\[
BW = \frac{32}{0.040}
\]
\[
BW = 800\ MB/s
\]
This is the combined read-plus-write bandwidth, not the bandwidth of either operation individually.
2.5 Random access throughput test
Objective: Measure SDRAM performance under non-sequential access conditions.

This test is particularly useful for:
- Operating systems.
- File systems.
- Networking stacks.
- Dynamic memory allocation.
- Data structures.
- Applications with irregular memory access patterns.
Implementation:
1. Allocate a large SDRAM buffer.
2. Generate a pseudo-random sequence of addresses.
3. Read or write memory at those locations.
4. Measure execution time.
5. Repeat with different buffer sizes.
Test different access patterns:
Access pattern	Purpose
Sequential	Measure burst efficiency
Random	Measure irregular access performance
Strided	Evaluate access efficiency with fixed address gaps
Alternating regions	Evaluate bank and row switching effects
Cache-line aligned	Evaluate aligned access performance
Unaligned	Evaluate unaligned access overhead


Random access throughput will generally be lower than sequential throughput because of SDRAM row activation, precharge and access latency.
2.6 DMA-based throughput test
Objective: Measure SDRAM bandwidth using DMA instead of CPU-driven memory accesses.

This is particularly important in embedded systems where peripherals and DMA engines frequently access SDRAM.

Recommended DMA tests:
Test	Source	Destination	Purpose
DMA Memory-to-Memory	SDRAM	SDRAM	Measure DMA copy bandwidth
DMA Memory-to-Peripheral	SDRAM	Peripheral	Measure memory read performance under DMA
DMA Peripheral-to-Memory	Peripheral	SDRAM	Measure memory write performance under DMA
Multiple DMA channels	SDRAM	Multiple destinations	Evaluate contention
CPU + DMA	SDRAM	SDRAM	Evaluate simultaneous CPU and DMA traffic


For DMA testing:
- Use sufficiently large transfers.
- Align buffers according to DMA requirements.
- Handle cache coherency correctly.
- Measure actual transfer completion, not just DMA submission time.
- Record transfer size and DMA configuration.
DMA testing is especially valuable because CPU-only measurements cannot represent all SDRAM traffic generated by the system.
2.7 Multi-master contention test
Objective: Evaluate SDRAM performance when multiple system components access external memory simultaneously.

Typical bus masters include:
- CPU.
- DMA controller.
- GPU.
- Display controller.
- Ethernet controller.
- USB controller.
- Camera interface.
- DSP or accelerator.
Run different combinations of concurrent memory accesses.

For example:
Scenario	CPU	DMA	Display	Objective
1	Read	Idle	Idle	Baseline CPU read
2	Write	Idle	Idle	Baseline CPU write
3	Read	Read/Write	Idle	CPU and DMA contention
4	Read	Read/Write	Active	Display-related contention
5	Read/Write	Read/Write	Active	Maximum system load


Measure both aggregate bandwidth and individual master performance if the memory controller provides suitable performance counters.

This test helps identify memory controller arbitration issues and performance degradation under real application workloads.
3. How to measure SDRAM throughput
The basic measurement formula is:
\[
Throughput = \frac{Total\ Bytes\ Transferred}{Time}
\]
For MB/s:
\[
Throughput =
\frac{Total\ Bytes}{Elapsed\ Time\ (seconds)}
\times \frac{1}{1024^2}
\]
For GB/s:
\[
Throughput =
\frac{Total\ Bytes}{Elapsed\ Time\ (seconds)}
\times \frac{1}{10^9}
\]
3.1 Measurement methods
There are three useful approaches.
Method	Description	Recommended usage
CPU timer	Measure the time taken by CPU read/write operations	Bare-metal and RTOS
Hardware performance counters	Measure memory controller transactions and cycles	SoCs with performance monitoring
Logic analyzer / oscilloscope	Observe SDRAM bus activity and timing	Hardware debugging and validation


My recommendation: Use CPU timers for initial benchmarking, hardware performance counters for accurate system-level analysis, and external instruments when investigating signal integrity or SDRAM timing issues.
3.2 Hardware performance counters
If your SoC provides SDRAM or DDR controller performance counters, use them.

Useful counters include:
- Total read bytes.
- Total write bytes.
- Read transaction count.
- Write transaction count.
- Bus utilization.
- Memory controller busy cycles.
- Refresh cycles.
- Read/write turnaround cycles.
- Row hit and row miss counts, if supported.
The actual counter names and availability depend on the SoC and memory controller.

Calculate:
\[
Read\ BW =
\frac{Total\ Read\ Bytes}{Measurement\ Time}
\]
\[
Write\ BW =
\frac{Total\ Write\ Bytes}{Measurement\ Time}
\]
\[
Aggregate\ BW =
\frac{Read\ Bytes + Write\ Bytes}{Measurement\ Time}
\]
This is generally more useful than measuring CPU execution time alone when trying to understand actual external memory traffic.
4. Benchmarking using standard tools
Depending on the software environment, you can use existing benchmarking tools rather than implementing everything from scratch.
4.1 Linux-based embedded systems
For Linux-based embedded platforms, I recommend:
Tool	Purpose
STREAM	Sequential memory bandwidth
lmbench	Memory latency and bandwidth characterization
perf	CPU performance counters
Custom C benchmark	Read/write/copy and access-pattern testing
DDR controller counters	Actual memory controller traffic


STREAM benchmark

STREAM is particularly useful for evaluating memory bandwidth.

It implements four operations:
Benchmark	Operation	What it measures
Copy	a[i] = b[i]	Read + write bandwidth
Scale	a[i] = scalar * b[i]	Read + write with computation
Add	a[i] = b[i] + c[i]	Multiple reads + write
Triad	a[i] = b[i] + scalar * c[i]	Multiple reads + write with computation


STREAM is designed to minimize data reuse from cache and measure memory subsystem performance. Texas Instruments

For example:
gcc -O3 -fopenmp stream.c -o stream

./stream

Make sure the array size is sufficiently large to exceed the effective CPU cache capacity.

For an embedded Linux system, TI also documents STREAM-based memory benchmarking in its Processor SDK performance guides.

Reference: TI Processor SDK Performance Guide
4.2 Bare-metal and RTOS-based systems
For bare-metal or RTOS environments, implement a dedicated SDRAM benchmark application.

Recommended components:
SDRAM Benchmark Application
          |
          +---- Memory Initialization
          |
          +---- Memory Integrity Tests
          |
          +---- Sequential Read Test
          |
          +---- Sequential Write Test
          |
          +---- Read/Write Test
          |
          +---- Random Access Test
          |
          +---- DMA Throughput Test
          |
          +---- Concurrent Access Test
          |
          +---- Result Calculation
          |
          +---- Report Generation

Use the following hardware resources:
- Hardware timer for elapsed time.
- CPU cycle counter, if available.
- DMA controller.
- SDRAM controller performance counters.
- UART or console for reporting.
For Cortex-M-based systems, the DWT cycle counter is useful for measuring CPU execution cycles, provided it is implemented and enabled on the particular core.

For Cortex-A-based systems, use the architectural performance monitoring unit where available.
5. How to validate the measured results
This is one of the most important parts of SDRAM testing.

A benchmark reporting 700 MB/s does not automatically mean that SDRAM is operating correctly.

You need to compare measured performance against theoretical bandwidth and expected system performance.
5.1 Calculate theoretical versus measured bandwidth
Consider this example:
Parameter	Value
SDRAM type	DDR
Data bus width	32 bits
Data rate	400 MT/s
Theoretical bandwidth	1600 MB/s


Suppose the benchmark produces:
Test	Measured bandwidth
Sequential read	1200 MB/s
Sequential write	1100 MB/s
Memory copy	1000 MB/s
Random read	250 MB/s
DMA copy	1250 MB/s


Calculate efficiency:
\[
Efficiency(\%) =
\frac{Measured\ Bandwidth}{Theoretical\ Bandwidth}
\times 100
\]
For sequential read:
\[
Efficiency =
\frac{1200}{1600}\times100
\]
\[
Efficiency = 75\%
\]
This indicates that the benchmark achieves 75% of the theoretical maximum bandwidth.
5.2 Suggested performance acceptance criteria
The following are starting targets for engineering evaluation, not universal SDRAM specifications.
Test	Suggested target
Sequential read	Establish a baseline; aim for high utilization of theoretical bandwidth
Sequential write	Establish a baseline; account for write buffering and cache behavior
Memory copy	Evaluate combined read/write bandwidth
Random access	Compare against the platform baseline
DMA transfer	Validate against the DMA and memory-controller capabilities
Concurrent access	Check that critical masters meet their bandwidth requirements
Long-duration stress	Zero data corruption or transfer failures


For a well-configured memory subsystem, Arm's ASCT documentation describes 85–95% of theoretical peak as a useful reference for peak bandwidth validation. This is not a guaranteed target for every embedded SoC or benchmark configuration. Arm Learn
5.3 Validate consistency
Run every benchmark multiple times.

For example:
Iteration	Read bandwidth
1	1180 MB/s
2	1190 MB/s
3	1175 MB/s
4	1185 MB/s
5	1192 MB/s


Calculate:
- Minimum bandwidth.
- Maximum bandwidth.
- Average bandwidth.
- Standard deviation.
- Percentage variation.
A large variation may indicate:
- CPU frequency changes.
- Cache effects.
- Background processes.
- Memory controller contention.
- Thermal throttling.
- Interrupt overhead.
Arm recommends repeated benchmark execution and investigating variations greater than approximately 5% in its memory characterization guidance. Arm Learn
6. Important factors that can affect SDRAM throughput
When benchmarking, pay particular attention to these factors.
Factor	Potential impact	Recommended action
CPU cache enabled	Benchmark may measure cache bandwidth instead of SDRAM bandwidth	Use datasets larger than cache or control cache attributes
CPU frequency	Changes achievable CPU-side bandwidth	Fix CPU frequency where possible
SDRAM clock	Directly affects theoretical bandwidth	Verify actual configured clock
SDRAM timing parameters	Affects latency and throughput	Verify timing against the memory datasheet
Burst length	Affects sequential access efficiency	Test supported burst configurations
Memory bank configuration	Affects parallelism and row conflicts	Evaluate different access patterns
Refresh interval	Refresh operations consume memory cycles	Verify refresh configuration
Bus width	Determines theoretical bandwidth	Verify hardware configuration
DMA configuration	Affects transfer efficiency	Evaluate burst size and outstanding transactions
Cache coherency	Can produce misleading measurements or incorrect data	Perform appropriate cache maintenance


Particularly important: Never compare two SDRAM throughput results without ensuring that cache configuration, clock frequency, memory controller settings and benchmark methodology are equivalent.
7. Recommended SDRAM validation test plan
For an embedded device, I would organize the testing into the following phases.
Phase	Test	Priority	Expected result
1	SDRAM initialization	Critical	Successful initialization
2	Data integrity tests	Critical	Zero data errors
3	Address bus test	Critical	Zero address errors
4	Sequential read	High	Expected read bandwidth
5	Sequential write	High	Expected write bandwidth
6	Memory copy	High	Expected combined bandwidth
7	Random access	Medium	Consistent access performance
8	DMA throughput	High	Expected DMA bandwidth
9	CPU + DMA contention	High	Stable concurrent performance
10	Long-duration stress	Critical	Zero corruption
11	Different access sizes	Medium	Characterized performance
12	Temperature and voltage variation	High	Stable operation within specified operating conditions


Suggested test configurations
Run the throughput benchmarks with different buffer sizes.
Buffer size	Purpose
4 KB	Small memory access
16 KB	Small working set
64 KB	Medium working set
256 KB	Larger working set
1 MB	Large working set
4 MB	External memory performance
16 MB	Sustained external memory performance
32 MB or larger	Large working-set characterization


Choose buffer sizes according to available SDRAM capacity and CPU cache size.

For each buffer size, test:
- 8-bit accesses.
- 16-bit accesses.
- 32-bit accesses.
- 64-bit accesses, if supported.
- Different burst lengths.
- Different memory alignment conditions.
This helps identify whether throughput is limited by the SDRAM controller, CPU load/store architecture, bus width or access pattern.
8. SDRAM benchmark report format
Every benchmark execution should generate a structured report.

I recommend the following format:
Parameter	Result
Board	Board name
Processor	SoC name
SDRAM type	DDR3 / DDR4 / LPDDR4 / SDR
SDRAM capacity	256 MB
Data bus width	32 bits
SDRAM clock	200 MHz
Data rate	400 MT/s
Theoretical bandwidth	1600 MB/s
CPU frequency	800 MHz
Cache configuration	Enabled / Disabled
Buffer size	16 MB
Test type	Sequential Read
Measured time	13.33 ms
Measured bandwidth	1200 MB/s
Efficiency	75%
Data integrity	PASS
Overall result	PASS


Also record:
- Test date and time.
- Firmware version.
- SDRAM controller configuration.
- SDRAM timing parameters.
- Number of iterations.
- Minimum, maximum and average throughput.
- Temperature and operating voltage, where available.
This makes results reproducible and useful for regression testing.
9. My recommendation: Build a dedicated SDRAM benchmark framework
For an embedded platform, I would develop a reusable SDRAM benchmarking framework that works independently of the application.

The framework could have the following architecture:
             SDRAM Benchmark Framework
                       |
          +------------+------------+
          |                         |
    Functional Tests          Performance Tests
          |                         |
     +----+----+              +-----+------+
     |    |    |              |     |      |
   Data Addr Random         Read  Write   Copy
   Test Test Pattern        BW    BW      BW
          |                         |
          +------------+------------+
                       |
                 DMA Tests
                       |
               Contention Tests
                       |
                Result Analysis
                       |
                Pass / Fail
                       |
                Report Generator

Recommended APIs
int sdram_init_test(void);

int sdram_data_bus_test(void);

int sdram_address_bus_test(void);

int sdram_pattern_test(void);

int sdram_random_test(void);

float sdram_read_bandwidth(uint32_t size);

float sdram_write_bandwidth(uint32_t size);

float sdram_copy_bandwidth(uint32_t size);

float sdram_random_bandwidth(uint32_t size);

float sdram_dma_bandwidth(uint32_t size);

int sdram_stress_test(uint32_t duration_seconds);

void sdram_generate_report(void);

The framework should support both bare-metal and RTOS environments, with platform-specific implementations for timers, cache management, DMA and memory controller counters.
10. Final recommendation: Which tests should you prioritize?
If your primary objective is to validate SDRAM performance on a newly designed embedded board, I would prioritize these eight tests:
No.	Test	What you establish
1	Data integrity test	SDRAM hardware is functioning correctly
2	Sequential read	Maximum CPU read performance
3	Sequential write	Maximum CPU write performance
4	Memory copy	Combined read/write performance
5	Random access	Non-sequential access performance
6	DMA memory-to-memory	DMA-driven SDRAM performance
7	CPU + DMA contention	Multi-master performance
8	Long-duration stress	Reliability and stability


One important distinction: Throughput testing and SDRAM signal-integrity testing are different activities. If you suspect hardware-level issues, such as incorrect clocking, signal integrity, setup/hold violations or marginal timing, you should additionally perform hardware-level validation using the appropriate oscilloscope, memory interface analysis tools and SDRAM controller diagnostics.
References
1. U-Boot Memory Test (mtest)
2. Arm Memory Subsystem Characterization
3. Arm Single-Core Memory Bandwidth Testing
4. Arm Multi-Core Memory Bandwidth Testing
5. TI Processor SDK Performance Guide