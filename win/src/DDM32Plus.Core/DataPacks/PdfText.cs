// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text;
using UglyToad.PdfPig;
using UglyToad.PdfPig.Content;

namespace DDM32Plus.Core.DataPacks;

/// <summary>
/// A PDF's text as lines (PdfPig, Apache-2.0): the words of each page grouped into lines by their baseline, top to
/// bottom, left to right, one space between words. datapack.py uses pymupdf's get_text() for the same; the AD 2.18
/// parser works on lines either way (a table row is one line here, so a service and its frequency share it; pymupdf
/// may put cells on lines of their own, which the "last named service above" rule covers).
/// </summary>
public static class PdfText
{
    public static string Read(string path)
    {
        try
        {
            using var doc = PdfDocument.Open(path);
            var sb = new StringBuilder();
            foreach (var page in doc.GetPages())
                sb.Append(PageText(page)).Append('\n');
            return sb.ToString();
        }
        catch (Exception e) when (e is not (OutOfMemoryException or IOException or UnauthorizedAccessException))
        {
            throw new PackError($"{Path.GetFileName(path)}: not a readable PDF ({e.Message})");
        }
    }

    private static string PageText(Page page)
    {
        var words = page.GetWords().Where(w => !string.IsNullOrWhiteSpace(w.Text)).ToList();
        // lines: words whose baselines lie within half a word height of the line's first word
        var lines = new List<(double Y, double H, List<Word> Words)>();
        foreach (var w in words.OrderByDescending(w => w.BoundingBox.Bottom).ThenBy(w => w.BoundingBox.Left))
        {
            var y = w.BoundingBox.Bottom;
            var h = Math.Max(1.0, w.BoundingBox.Height);
            var line = lines.FindIndex(l => Math.Abs(l.Y - y) <= Math.Max(l.H, h) * 0.5);
            if (line < 0)
                lines.Add((y, h, [w]));
            else
                lines[line].Words.Add(w);
        }
        return string.Join("\n", lines.OrderByDescending(l => l.Y)
            .Select(l => string.Join(" ", l.Words.OrderBy(w => w.BoundingBox.Left).Select(w => w.Text))));
    }

    /// <summary>For the tests and the page: the PDF's page count (0 when it is no PDF).</summary>
    public static int Pages(string path)
    {
        try
        {
            using var doc = PdfDocument.Open(path);
            return doc.NumberOfPages;
        }
        catch (Exception e) when (e is not (OutOfMemoryException or IOException or UnauthorizedAccessException))
        {
            return 0;
        }
    }
}
