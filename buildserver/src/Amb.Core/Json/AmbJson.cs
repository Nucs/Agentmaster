using System.Text.Json.Serialization;
using Amb.Core.Allocation;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;

namespace Amb.Core.Json;

/// <summary>
/// The source-generated JSON metadata of every type the build server stores or sends (records, envelopes, API bodies).
/// </summary>
/// <remarks>
/// Source generation, not reflection: the guest agent is published with Native AOT, which allows no reflection, and
/// one context shared by every component means a record written by the coordinator is read back with exactly the
/// same names and converters by the CLI. Names are camelCase; enums are strings (each enum carries its converter);
/// missing optional fields are left out of the output.
/// </remarks>
[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    WriteIndented = true)]
[JsonSerializable(typeof(Envelope<RequestRecord>))]
[JsonSerializable(typeof(Envelope<BuildRecord>))]
[JsonSerializable(typeof(Envelope<SlotRecord>))]
[JsonSerializable(typeof(RequestRecord))]
[JsonSerializable(typeof(BuildRecord))]
[JsonSerializable(typeof(SlotRecord))]
[JsonSerializable(typeof(RequestEvent))]
[JsonSerializable(typeof(Placement))]
[JsonSerializable(typeof(SiteFree))]
public sealed partial class AmbJson : JsonSerializerContext;

/// <summary>The same metadata without indentation, for one-line forms: event-log lines and API bodies.</summary>
[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    WriteIndented = false)]
[JsonSerializable(typeof(RequestRecord))]
[JsonSerializable(typeof(BuildRecord))]
[JsonSerializable(typeof(SlotRecord))]
[JsonSerializable(typeof(RequestEvent))]
[JsonSerializable(typeof(Placement))]
[JsonSerializable(typeof(SiteFree))]
[JsonSerializable(typeof(SlotConfig))]
[JsonSerializable(typeof(GuestHello))]
[JsonSerializable(typeof(GuestHeartbeat))]
[JsonSerializable(typeof(HeartbeatReply))]
[JsonSerializable(typeof(GuestStep))]
[JsonSerializable(typeof(StepResult))]
[JsonSerializable(typeof(StepLog))]
[JsonSerializable(typeof(CreateRequest))]
[JsonSerializable(typeof(EventPage))]
[JsonSerializable(typeof(CoordinatorStatus))]
[JsonSerializable(typeof(HostFactsReport))]
[JsonSerializable(typeof(List<RequestRecord>))]
[JsonSerializable(typeof(List<BuildRecord>))]
public sealed partial class AmbJsonLine : JsonSerializerContext;
