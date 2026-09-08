import { Component, computed, OnInit, signal } from '@angular/core';

type EntryKind = 'directory' | 'file';

interface FileEntry {
  kind: EntryKind;
  modified: Date;
  name: string;
  size: number;
}

interface FileListResponse {
  entries: Array<{
    modified: number;
    name: string;
    size: number;
    type: EntryKind;
  }>;
}

@Component({
  selector: 'app-file-explorer',
  templateUrl: './file-explorer.component.html',
  styleUrl: './file-explorer.component.scss',
})
export class FileExplorerComponent implements OnInit {
  protected readonly currentPath = signal('/');
  protected readonly entries = signal<FileEntry[]>([]);
  protected readonly newDirectoryName = signal('');
  protected readonly selectedEntry = signal<FileEntry | null>(null);
  protected readonly status = signal('Loading /sdcard...');
  protected readonly pathLabel = computed(() => `/sdcard${this.currentPath()}`);

  ngOnInit(): void {
    void this.loadDirectory();
  }

  protected selectEntry(entry: FileEntry): void {
    this.selectedEntry.set(entry);
    this.status.set(`${entry.name} selected.`);
  }

  protected openEntry(entry: FileEntry): void {
    if (entry.kind !== 'directory') {
      this.selectEntry(entry);
      return;
    }

    this.currentPath.set(this.joinPath(entry.name));
    this.selectedEntry.set(null);
    void this.loadDirectory();
  }

  protected navigateUp(): void {
    const path = this.currentPath();
    const separator = path.lastIndexOf('/');
    this.currentPath.set(separator === 0 ? '/' : path.slice(0, separator));
    this.selectedEntry.set(null);
    void this.loadDirectory();
  }

  protected async createDirectory(): Promise<void> {
    const name = this.newDirectoryName().trim();
    if (!this.isValidName(name)) {
      this.status.set('Enter a valid directory name.');
      return;
    }

    try {
      await this.request(`/api/directories?path=${encodeURIComponent(this.joinPath(name))}`, {
        method: 'POST',
      });
      this.newDirectoryName.set('');
      await this.loadDirectory(`Created ${name}.`);
    } catch (error) {
      this.showError(error);
    }
  }

  protected async uploadFiles(event: Event): Promise<void> {
    const input = event.target as HTMLInputElement;
    const files = Array.from(input.files ?? []);
    if (files.length === 0) {
      return;
    }

    try {
      for (const file of files) {
        await this.request(`/api/files?path=${encodeURIComponent(this.joinPath(file.name))}`, {
          body: file,
          method: 'PUT',
        });
      }
      input.value = '';
      await this.loadDirectory(`Uploaded ${files.length} file${files.length === 1 ? '' : 's'}.`);
    } catch (error) {
      this.showError(error);
    }
  }

  protected async deleteSelected(): Promise<void> {
    const entry = this.selectedEntry();
    if (entry === null) {
      return;
    }
    if (entry.kind === 'directory') {
      this.status.set('Directory deletion is not supported.');
      return;
    }

    try {
      await this.request(`/api/files?path=${encodeURIComponent(this.joinPath(entry.name))}`, {
        method: 'DELETE',
      });
      this.selectedEntry.set(null);
      await this.loadDirectory(`Deleted ${entry.name}.`);
    } catch (error) {
      this.showError(error);
    }
  }

  protected isSelected(entry: FileEntry): boolean {
    return this.selectedEntry()?.name === entry.name;
  }

  protected formatSize(size: number): string {
    if (size < 1024) {
      return `${size} B`;
    }
    return `${(size / 1024).toFixed(1)} KB`;
  }

  protected formatModified(modified: Date): string {
    return modified.toLocaleString();
  }

  private async loadDirectory(successMessage?: string): Promise<void> {
    this.status.set('Loading...');
    try {
      const response = await this.request(
        `/api/files?path=${encodeURIComponent(this.currentPath())}`,
      );
      const data = (await response.json()) as FileListResponse;
      this.entries.set(
        data.entries.map((entry) => ({
          kind: entry.type,
          modified: new Date(entry.modified * 1000),
          name: entry.name,
          size: entry.size,
        })),
      );
      this.status.set(successMessage ?? `${data.entries.length} item${data.entries.length === 1 ? '' : 's'}.`);
    } catch (error) {
      this.entries.set([]);
      this.showError(error);
    }
  }

  private joinPath(name: string): string {
    return this.currentPath() === '/' ? `/${name}` : `${this.currentPath()}/${name}`;
  }

  private isValidName(name: string): boolean {
    return name.length > 0 && !name.includes('/') && name !== '.' && name !== '..';
  }

  private async request(url: string, init?: RequestInit): Promise<Response> {
    const response = await fetch(url, init);
    if (!response.ok) {
      throw new Error(await response.text());
    }
    return response;
  }

  private showError(error: unknown): void {
    this.status.set(error instanceof Error ? error.message : 'The file operation failed.');
  }
}
