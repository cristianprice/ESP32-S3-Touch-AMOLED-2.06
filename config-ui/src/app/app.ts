import { Component, signal } from '@angular/core';
import { FileExplorerComponent } from './file-explorer/file-explorer.component';
import { Tab, TabsComponent } from './tabs/tabs.component';

@Component({
  imports: [TabsComponent, FileExplorerComponent],
  selector: 'app-root',
  styleUrl: './app.scss',
  templateUrl: './app.html',
})
export class App {
  protected readonly tabs: Tab[] = [
    { id: 'files', label: 'Files' },
    { id: 'device', label: 'Device' },
  ];
  protected readonly activeTab = signal('files');
}
