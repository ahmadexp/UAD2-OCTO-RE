PAPER_NAME := when-gpus-do-not-perform-best-study-design
BUILD_DIR := ../tmp/pdfs/$(PAPER_NAME)
OUTPUT_DIR := ../output/pdf
OUTPUT_PDF := $(OUTPUT_DIR)/$(PAPER_NAME).pdf

.PHONY: all clean

all: $(OUTPUT_PDF)

$(OUTPUT_PDF): main.tex references.bib ../docs/images/uad2-octo-rev5-highres.jpg
	mkdir -p $(BUILD_DIR) $(OUTPUT_DIR)
	latexmk -pdf -interaction=nonstopmode -halt-on-error \
		-outdir=$(BUILD_DIR) main.tex
	cp $(BUILD_DIR)/main.pdf $(OUTPUT_PDF)

clean:
	latexmk -C -outdir=$(BUILD_DIR) main.tex
	rm -f $(OUTPUT_PDF)
