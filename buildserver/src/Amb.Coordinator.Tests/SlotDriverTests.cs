using Amb.Coordinator.Slots;
using Xunit;

namespace Amb.Coordinator.Tests;

/// <summary>The podman driver's reading of an image's dockur state, which decides whether a boot reinstalls Windows.</summary>
public sealed class SlotDriverTests : IDisposable
{
    /// <summary>The test's own image folder under %TEMP%, deleted after it.</summary>
    private readonly string _image = Path.Combine(Path.GetTempPath(), "amb-image-" + Guid.NewGuid().ToString("N"));

    /// <summary>Makes the image folder.</summary>
    public SlotDriverTests() => Directory.CreateDirectory(_image);

    /// <summary>Deletes the image folder.</summary>
    public void Dispose() => Directory.Delete(_image, recursive: true);

    /// <summary>The version is the ISO's name dockur recorded, without ".iso" (what a boot must pass as VERSION).</summary>
    /// <param name="recorded">The windows.base content dockur wrote.</param>
    /// <param name="expected">The VERSION a boot passes.</param>
    [Theory]
    [InlineData("core11.iso\n", "core11")]
    [InlineData("win11x64.iso", "win11x64")]
    [InlineData("  tiny11.ISO \r\n", "tiny11")]
    public void VersionComesFromWindowsBase(string recorded, string expected)
    {
        File.WriteAllText(Path.Combine(_image, "windows.base"), recorded);
        Assert.Equal(expected, PodmanSlotDriver.ImageVersion(_image));
    }

    /// <summary>An image without the record, or with one that names no plain version, is refused: booting it with a
    /// guessed VERSION would make dockur reinstall over the slot.</summary>
    /// <param name="recorded">The windows.base content, or null for no file.</param>
    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("core11")]
    [InlineData(".iso")]
    [InlineData("core11.iso -e EVIL=1.iso")]
    [InlineData("../x.iso")]
    public void MissingOrOddRecordIsRefused(string? recorded)
    {
        if (recorded is not null)
        {
            File.WriteAllText(Path.Combine(_image, "windows.base"), recorded);
        }
        Assert.Throws<InvalidOperationException>(() => PodmanSlotDriver.ImageVersion(_image));
    }

    /// <summary>An uncapped slot's guest sees its grant; so does one whose guest RAM is not above its grant.</summary>
    /// <param name="guestMiB">The spec's guest RAM, or null for none.</param>
    [Theory]
    [InlineData(null)]
    [InlineData(8192L)]
    [InlineData(4096L)]
    public void UncappedSlotSeesItsGrant(long? guestMiB)
    {
        SlotSpec spec = Spec(8192, guestMiB);
        Assert.Equal(["-e", "RAM_SIZE=8192M"], PodmanSlotDriver.MemoryArguments(spec));
    }

    /// <summary>A capped slot (DESIGN.md §16.14): the guest sees its own RAM, the container keeps at most the grant
    /// resident and may swap the rest plus a margin, and dockur's RAM check is off.</summary>
    [Fact]
    public void CappedSlotIsLimitedToItsGrant()
    {
        SlotSpec spec = Spec(2560, 4096);
        Assert.Equal(
            ["-e", "RAM_SIZE=4096M", "-e", "RAM_CHECK=N", "--memory", "2560m", "--memory-swap", "7680m"],
            PodmanSlotDriver.MemoryArguments(spec));
    }

    /// <summary>A slot spec with the given grant and guest RAM (the other fields do not size memory).</summary>
    /// <param name="grantMiB">The grant's memory.</param>
    /// <param name="guestMiB">The guest's RAM, or null.</param>
    /// <returns>The spec.</returns>
    private SlotSpec Spec(long grantMiB, long? guestMiB) =>
        new("s1", "am-e2e", new Amb.Core.Allocation.Resources(grantMiB, 4, 1024), _image, _image, _image,
            GuestMemoryMiB: guestMiB);
}
