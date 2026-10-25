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
}
