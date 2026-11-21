// SPDX-License-Identifier: GPL-3.0-or-later
//! A PDF's text as lines (pdf-extract on lopdf, MIT; DataPacks/PdfText.cs used PdfPig): the characters of each page
//! grouped into words (a gap or a space ends a word) and the words into lines by their baseline, top to bottom, left
//! to right, one space between words. The AD 2.18 parser works on lines (a table row is one line here, so a service
//! and its frequency share it).

use super::PackError;
use pdf_extract::{MediaBox, OutputDev, OutputError, Transform};
use std::path::Path;

#[derive(Debug, Clone)]
struct Word {
    x: f64,
    y: f64,
    h: f64,
    end: f64,
    text: String,
}

#[derive(Default)]
struct Collector {
    pages: Vec<Vec<Word>>,
    cur: Option<Word>,
}

impl Collector {
    fn end_word(&mut self) {
        if let Some(w) = self.cur.take() {
            if !w.text.trim().is_empty() {
                if let Some(p) = self.pages.last_mut() {
                    p.push(w);
                }
            }
        }
    }
}

impl OutputDev for Collector {
    fn begin_page(&mut self, _n: u32, _m: &MediaBox, _a: Option<(f64, f64, f64, f64)>) -> Result<(), OutputError> {
        self.pages.push(Vec::new());
        Ok(())
    }
    fn end_page(&mut self) -> Result<(), OutputError> {
        self.end_word();
        Ok(())
    }
    fn output_character(
        &mut self,
        trm: &Transform,
        width: f64,
        _spacing: f64,
        font_size: f64,
        ch: &str,
    ) -> Result<(), OutputError> {
        let (x, y) = (trm.m31, trm.m32);
        let (vx, vy) = (font_size * (trm.m11 + trm.m21), font_size * (trm.m12 + trm.m22));
        let size = (vx * vy).abs().sqrt().max(1.0);
        let end = x + width * size;
        if ch.chars().all(char::is_whitespace) {
            self.end_word();
            return Ok(());
        }
        let continues =
            self.cur.as_ref().is_some_and(|w| (w.y - y).abs() <= size * 0.5 && x - w.end <= size * 0.15 && x >= w.x);
        if !continues {
            self.end_word();
            self.cur = Some(Word { x, y, h: size, end, text: String::new() });
        }
        let w = self.cur.as_mut().unwrap();
        w.text.push_str(ch);
        w.end = end;
        Ok(())
    }
    fn begin_word(&mut self) -> Result<(), OutputError> {
        Ok(())
    }
    fn end_word(&mut self) -> Result<(), OutputError> {
        Collector::end_word(self);
        Ok(())
    }
    fn end_line(&mut self) -> Result<(), OutputError> {
        Collector::end_word(self);
        Ok(())
    }
}

fn page_text(words: &[Word]) -> String {
    let mut sorted = words.to_vec();
    sorted.sort_by(|a, b| b.y.partial_cmp(&a.y).unwrap().then(a.x.partial_cmp(&b.x).unwrap()));
    // lines: words whose baselines lie within half a word height of the line's first word
    let mut lines: Vec<(f64, f64, Vec<Word>)> = Vec::new();
    for w in sorted {
        match lines.iter_mut().find(|l| (l.0 - w.y).abs() <= l.1.max(w.h) * 0.5) {
            Some(l) => l.2.push(w),
            None => lines.push((w.y, w.h, vec![w])),
        }
    }
    lines.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap());
    lines
        .into_iter()
        .map(|(_, _, mut ws)| {
            ws.sort_by(|a, b| a.x.partial_cmp(&b.x).unwrap());
            ws.iter().map(|w| w.text.as_str()).collect::<Vec<_>>().join(" ")
        })
        .collect::<Vec<_>>()
        .join("\n")
}

fn load(path: &Path) -> Result<Vec<Vec<Word>>, String> {
    let bytes = std::fs::read(path).map_err(|e| e.to_string())?;
    // pdf-extract may panic on a broken file: such a file is "not a readable PDF", never a crash
    std::panic::catch_unwind(move || -> Result<Vec<Vec<Word>>, String> {
        let doc = lopdf::Document::load_mem(&bytes).map_err(|e| e.to_string())?;
        let mut c = Collector::default();
        pdf_extract::output_doc(&doc, &mut c).map_err(|e| e.to_string())?;
        c.end_word();
        Ok(c.pages)
    })
    .map_err(|_| "the PDF reader stopped".to_string())?
}

/// The PDF's text, one line a table row, pages separated by "\n".
pub fn read(path: &Path) -> Result<String, PackError> {
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let pages = load(path).map_err(|e| PackError(format!("{name}: not a readable PDF ({e})")))?;
    let mut s = String::new();
    for p in pages {
        s.push_str(&page_text(&p));
        s.push('\n');
    }
    Ok(s)
}

/// The PDF's page count (0 when it is no PDF).
pub fn pages(path: &Path) -> usize {
    std::fs::read(path)
        .ok()
        .and_then(|b| {
            std::panic::catch_unwind(move || lopdf::Document::load_mem(&b).ok().map(|d| d.get_pages().len())).ok()
        })
        .flatten()
        .unwrap_or(0)
}
